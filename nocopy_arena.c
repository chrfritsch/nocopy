/* nocopy arena: segments, shards, slot tables, pins, writes and repair. No PHP in here. */

#include "nocopy_arena_int.h"
#include <sched.h>
#include <stdlib.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

nocopy_arena *nocopy_a;
char *nocopy_seg_base;
unsigned nocopy_seg_shift;
uint32_t nocopy_shard_mask, nocopy_tab_mask;
nocopy_slot *nocopy_tabs[NOCOPY_MAX_SHARDS];
uint32_t *nocopy_ghosts[NOCOPY_MAX_SHARDS];
uint64_t nocopy_self;
size_t nocopy_seg_size;

static time_t checked[NOCOPY_MAX_WORKERS]; /* last liveness check of a pinning worker: at most one a second */

/* ---- locks ---- */

static void shard_repair(nocopy_shard *sh, uint64_t dead);
static void wip_abort(int slot);

/* Slow path of nocopy_lock(): test-and-test-and-set holding the owner id; yields once the wait gets long, and takes the lock over from a dead holder.
 * Whatever that holder was in the middle of is then repaired (shard_repair).
 * Robust pthread mutexes were measured 2-8x slower under contention (futex sleeps); this keeps the spin. */
void nocopy_lock_wait(nocopy_shard *sh)
{
	uint64_t *l = &sh->lock;
	for (unsigned spins = 0;;) {
		uint64_t h = __atomic_load_n(l, __ATOMIC_RELAXED);
		if (!h) {
			if (__atomic_compare_exchange_n(l, &h, nocopy_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				return;
			}
		} else if (++spins < 128) {
			nocopy_cpu_relax();
		} else {
			sched_yield();
			if (!(spins & 1023) && h != nocopy_self && nocopy_owner_dead(h)
			 && __atomic_compare_exchange_n(l, &h, nocopy_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				__atomic_add_fetch(&nocopy_a->lock_steals, 1, __ATOMIC_RELAXED);
				shard_repair(sh, h);
				return;
			}
		}
	}
}

/* ---- workers and pins ---- */

static void bits_clear(int slot)
{
	for (int w = 0; w < NOCOPY_SEG_WORDS; w++) {
		__atomic_store_n(&nocopy_a->workers[slot].bits[w], 0, __ATOMIC_SEQ_CST);
	}
}

bool nocopy_claim(nocopy_ctx *c)
{
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < NOCOPY_MAX_WORKERS; i++) {
			uint64_t o = __atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED);
			if (pass ? !o || !nocopy_owner_dead(o) : o != 0) {
				continue;
			}
			if (__atomic_compare_exchange_n(&nocopy_a->workers[i].owner, &o, nocopy_self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				bits_clear(i);
				wip_abort(i); /* its previous owner died filling an entry */
				uint32_t cur = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_RELAXED);
				while (cur < (uint32_t) i + 1
				 && !__atomic_compare_exchange_n(&nocopy_a->nworkers, &cur, i + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
				c->slot = i;
				c->rng = ((uint32_t) (nocopy_self ^ (nocopy_self >> 32)) * 2654435761u + (uint32_t) i) | 1;
				return true;
			}
		}
	}
	c->slot = -1;
	return false;
}

void nocopy_release(nocopy_ctx *c)
{
	if (c->slot >= 0) {
		bits_clear(c->slot);
		__atomic_store_n(&nocopy_a->workers[c->slot].owner, 0, __ATOMIC_SEQ_CST);
		c->slot = -1;
	}
}

/* A forked child gets its own owner id and worker slot, and inherits the parent's pins. */
void nocopy_after_fork(nocopy_ctx *c)
{
	nocopy_self = nocopy_owner_self();
	if (nocopy_claim(c)) {
		for (int w = 0; w < NOCOPY_SEG_WORDS; w++) {
			__atomic_store_n(&nocopy_a->workers[c->slot].bits[w], c->mine[w], __ATOMIC_SEQ_CST);
		}
	}
}

/* Drop the kept pins: the caller promises no earlier zero-copy result is used again. */
void nocopy_keep_release(nocopy_ctx *c)
{
	memset(c->keep, 0, sizeof c->keep);
	if (!c->pin_refs) {
		c->transient = true;
		nocopy_drop_transient(c);
	}
}

/* ---- segments (shard locked) ---- */

static void seg_set(uint32_t s, uint8_t st) { __atomic_store_n(&nocopy_a->segs[s].state, st, __ATOMIC_RELEASE); }

static void seg_check(nocopy_shard *sh, uint32_t s)
{
	nocopy_seg *g = &nocopy_a->segs[s];
	if (g->state == SEG_SEALED && !g->live && !g->pending) {
		seg_set(s, SEG_DEAD);
		sh->ndead++;
		sh->hint = true;
	}
}

static void seg_free(nocopy_shard *sh, uint32_t s)
{
	uint32_t k = nocopy_a->segs[s].run;
#ifdef NOCOPY_POISON
	memset(seg_start(s), 0x5A, (size_t) k << nocopy_seg_shift); /* make use-after-free loud */
#endif
	for (uint32_t j = s; j < s + k; j++) {
		uint32_t gen = nocopy_a->segs[j].gen + 1;
		memset(&nocopy_a->segs[j], 0, sizeof(nocopy_seg)); /* SEG_FREE */
		nocopy_a->segs[j].gen = gen;
	}
	sh->nfree += k;
	sh->ndead--;
}

/* Hand back sh's dead segments that no live worker pins. */
static void reclaim(nocopy_shard *sh)
{
	sh->hint = false;
	if (!sh->ndead) {
		return;
	}
	/* Dead set first, pins second: a reader pins, then re-checks its slot; a segment that died after we read the pins
	 * could have been pinned (and re-checked) just before. DEAD only ends here, under this lock. */
	uint64_t dead[NOCOPY_SEG_WORDS] = {0};
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if (nocopy_a->segs[s].state == SEG_DEAD) {
			dead[s >> 6] |= (uint64_t) 1 << (s & 63);
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	uint64_t held[NOCOPY_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	time_t now = time(NULL);
	for (uint32_t i = 0; i < nw; i++) {
		uint64_t b[NOCOPY_SEG_WORDS], blocks = 0;
		for (int w = 0; w < NOCOPY_SEG_WORDS; w++) {
			b[w] = __atomic_load_n(&nocopy_a->workers[i].bits[w], __ATOMIC_SEQ_CST);
			blocks |= b[w] & dead[w];
		}
		if (!blocks) {
			continue;
		}
		uint64_t o = __atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o && o != nocopy_self && __atomic_exchange_n(&checked[i], now, __ATOMIC_RELAXED) != now) {
			if (nocopy_owner_dead(o)) {
				bits_clear(i); /* died holding pins */
				continue;
			}
		}
		for (int w = 0; w < NOCOPY_SEG_WORDS; w++) {
			held[w] |= b[w];
		}
	}
	bool blocked = false;
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if ((dead[s >> 6] & ~held[s >> 6]) >> (s & 63) & 1) {
			seg_free(sh, s);
		} else if (dead[s >> 6] >> (s & 63) & 1) {
			blocked = true;
		}
	}
	if (blocked) {
		__atomic_add_fetch(&nocopy_a->unpin_gen, 1, __ATOMIC_RELAXED); /* ask readers to drop idle pins */
	}
}

/* Inline here (the write path), exported for nocopy_evict.c. */
static inline __attribute__((always_inline)) void entry_die(nocopy_shard *sh, nocopy_entry *e)
{
	uint32_t s = nocopy_seg_of(e);
	e->live = false;
	nocopy_a->segs[s].live -= e->size;
	sh->live_bytes -= e->size;
	seg_check(sh, s);
}

static inline __attribute__((always_inline)) void entry_birth(nocopy_shard *sh, nocopy_entry *e)
{
	uint32_t s = nocopy_seg_of(e);
	e->live = true;
	nocopy_a->segs[s].pending--;
	nocopy_a->segs[s].live += e->size;
	sh->live_bytes += e->size;
}

void nocopy_entry_die(nocopy_shard *sh, nocopy_entry *e) { entry_die(sh, e); }
void nocopy_entry_birth(nocopy_shard *sh, nocopy_entry *e) { entry_birth(sh, e); }

void nocopy_abort(nocopy_shard *sh, nocopy_entry *e)
{
	uint32_t s = nocopy_seg_of(e);
	nocopy_a->segs[s].pending--;
	seg_check(sh, s);
}

int nocopy_ttl_bucket(time_t ttl)
{
	int b = ttl ? 1 : 0;
	while (b && b < NOCOPY_TTL_BUCKETS - 1 && ttl >= ((time_t) 1 << (3 * b))) {
		b++;
	}
	return b;
}

static inline __attribute__((always_inline)) nocopy_entry *seg_carve(uint32_t s, size_t total, time_t exp)
{
	nocopy_seg *g = &nocopy_a->segs[s];
	nocopy_entry *e = (nocopy_entry *) (seg_start(s) + g->used);
	e->size = total; /* before used covers it: a repair walk must never meet a header without a size */
	e->live = false;
	e->hot = 0;
	g->used += total;
	g->pending++;
	if (exp > g->max_exp) {
		g->max_exp = exp;
	}
	return e;
}

/* k contiguous free segments of sh, leaving `spare` free ones; the first gets `state`. UINT32_MAX if none. */
static uint32_t take_run(nocopy_shard *sh, uint32_t k, uint32_t spare, uint8_t state)
{
	if (sh->nfree < k + spare) {
		return UINT32_MAX;
	}
	for (uint32_t s = sh->lo, n = 0; s < sh->hi; s++) {
		n = nocopy_a->segs[s].state == SEG_FREE ? n + 1 : 0;
		if (n == k) {
			uint32_t got = s + 1 - k;
			for (uint32_t j = got + 1; j <= s; j++) {
				seg_set(j, SEG_CONT);
			}
			nocopy_a->segs[got].run = k;
			nocopy_a->segs[got].born = sh->last_seq;
			seg_set(got, state);
			sh->nfree -= k;
			return got;
		}
	}
	return UINT32_MAX;
}

nocopy_entry *nocopy_seg_alloc(nocopy_shard *sh, size_t total, bool reserve, int b, time_t exp)
{
	uint32_t spare = reserve ? 0 : 1, got;
	if (total > nocopy_seg_size / 2) { /* a run of contiguous segments */
		got = take_run(sh, (uint32_t) ((total + nocopy_seg_size - 1) >> nocopy_seg_shift), spare, SEG_SEALED);
		return got == UINT32_MAX ? NULL : seg_carve(got, total, exp);
	}
	int32_t cur = sh->cur[b];
	if (cur >= 0 && nocopy_a->segs[cur].used + total <= nocopy_seg_size) {
		return seg_carve(cur, total, exp);
	}
	got = take_run(sh, 1, spare, SEG_OPEN);
	if (got == UINT32_MAX) {
		return NULL;
	}
	if (cur >= 0) {
		seg_set(cur, SEG_SEALED);
		seg_check(sh, cur);
	}
	nocopy_a->segs[got].bucket = (uint8_t) b;
	sh->cur[b] = (int32_t) got;
	return seg_carve(got, total, exp);
}

/* ---- slot table (shard locked) ---- */

static inline __attribute__((always_inline)) nocopy_slot *locate(nocopy_shard *sh, uint64_t h, const char *k, uint32_t klen, nocopy_entry **found)
{
	nocopy_slot *t = tab_of(sh);
	uint32_t tag = nocopy_slot_tag(h);
	*found = NULL;
	for (uint32_t i = tag & nocopy_tab_mask;; i = (i + 1) & nocopy_tab_mask) { /* never full: nlive <= cap < slots */
		nocopy_slot s = t[i];
		if (!s) {
			return &t[i];
		}
		if ((uint32_t) (s >> 32) == tag) {
			nocopy_entry *e = nocopy_slot_entry(s);
			if (e->h == h && e->klen == klen && nocopy_key_eq(nocopy_key(e), k, klen)) {
				*found = e;
				return &t[i];
			}
		}
	}
}

/* Backward-shift deletion: no tombstones. Readers that miss while a key moves back past them retry (mseq). */
static void slot_remove(nocopy_shard *sh, nocopy_slot *p)
{
	nocopy_slot *t = tab_of(sh);
	uint32_t m = nocopy_tab_mask, i = (uint32_t) (p - t);
	bool shifting = false;
	for (uint32_t j = (i + 1) & m;; j = (j + 1) & m) {
		nocopy_slot s = t[j];
		if (!s) {
			break;
		}
		uint32_t home = (uint32_t) (s >> 32) & m;
		if (((j - home) & m) >= ((j - i) & m)) { /* i lies between its home and j: move it back */
			if (!shifting) {
				shifting = true;
				__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELAXED);
				__atomic_thread_fence(__ATOMIC_RELEASE);
			}
			__atomic_store_n(&t[i], s, __ATOMIC_RELEASE);
			i = j;
		}
	}
	__atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
	if (shifting) {
		__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELEASE);
	}
	sh->nlive--;
}

nocopy_slot *nocopy_slot_of(nocopy_shard *sh, const nocopy_entry *e)
{
	nocopy_slot *t = tab_of(sh);
	for (uint32_t i = nocopy_slot_tag(e->h) & nocopy_tab_mask;; i = (i + 1) & nocopy_tab_mask) {
		if (!t[i]) {
			return NULL;
		}
		if (nocopy_slot_entry(t[i]) == e) {
			return &t[i];
		}
	}
}

void nocopy_entry_unlink(nocopy_shard *sh, nocopy_entry *e)
{
	nocopy_slot *p = nocopy_slot_of(sh, e);
	if (p) {
		slot_remove(sh, p);
	}
	nocopy_entry_die(sh, e);
}

/* ---- dead writers ---- */

/* Before unlocking to fill e outside the lock: record it, so the entry of a writer that dies meanwhile is rolled back. NULL after the commit. */
static void wip_set(nocopy_ctx *c, const nocopy_entry *e)
{
	__atomic_store_n(&nocopy_a->workers[c->slot].wip, e ? (uint32_t) (((const char *) e - (const char *) nocopy_a) >> 3) : 0, __ATOMIC_RELAXED);
}

static nocopy_entry *wip_entry(uint32_t w) { return (nocopy_entry *) ((char *) nocopy_a + ((size_t) w << 3)); }

static nocopy_shard *seg_shard(uint32_t s)
{
	for (uint32_t i = 0;; i++) {
		if (s < nocopy_a->shards[i].hi) {
			return &nocopy_a->shards[i];
		}
	}
}

/* sh locked. Aborts the unfinished entries in sh of writers that died outside the lock. */
static void wip_reap(nocopy_shard *sh)
{
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		uint32_t w = __atomic_load_n(&nocopy_a->workers[i].wip, __ATOMIC_RELAXED);
		if (!w || seg_shard(nocopy_seg_of(wip_entry(w))) != sh) {
			continue;
		}
		uint64_t o = __atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o != nocopy_self && (!o || nocopy_owner_dead(o))) {
			__atomic_store_n(&nocopy_a->workers[i].wip, 0, __ATOMIC_RELAXED);
			nocopy_abort(sh, wip_entry(w));
		}
	}
}

/* A worker slot just claimed from a dead owner: abort the entry it was filling. No lock held. */
static void wip_abort(int slot)
{
	uint32_t w = __atomic_load_n(&nocopy_a->workers[slot].wip, __ATOMIC_RELAXED);
	if (!w) {
		return;
	}
	nocopy_shard *sh = seg_shard(nocopy_seg_of(wip_entry(w)));
	nocopy_lock(sh);
	if (__atomic_load_n(&nocopy_a->workers[slot].wip, __ATOMIC_RELAXED) == w) { /* not reaped meanwhile */
		__atomic_store_n(&nocopy_a->workers[slot].wip, 0, __ATOMIC_RELAXED);
		nocopy_abort(sh, wip_entry(w));
	}
	nocopy_unlock(sh);
}

/* sh's lock was just taken from a dead holder. Rebuilds what it may have left half done from the slot table, which only
 * ever changes by single stores: duplicate slots and an odd mseq (a delete's shift), segment states (a run taken or freed
 * halfway, an open segment not yet installed), live/pending counts (an entry allocated, moved or unlinked halfway). */
static void shard_repair(nocopy_shard *sh, uint64_t dead)
{
	nocopy_slot *t = tab_of(sh);
	if (sh->mseq & 1) {
		__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELEASE); /* else readers retry every miss forever */
	}
	for (uint32_t i = 0; i <= nocopy_tab_mask; i++) {
		while (t[i] && nocopy_slot_of(sh, nocopy_slot_entry(t[i])) != &t[i]) {
			slot_remove(sh, &t[i]); /* a key the shift copied back but did not yet clear */
		}
	}
	sh->nlive = 0;
	for (uint32_t i = 0; i <= nocopy_tab_mask; i++) {
		sh->nlive += t[i] != 0;
	}
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		int32_t c = sh->cur[b];
		if (c >= 0 && ((uint32_t) c < sh->lo || (uint32_t) c >= sh->hi || nocopy_a->segs[c].state != SEG_OPEN)) {
			sh->cur[b] = -1;
		}
	}
	sh->nfree = sh->ndead = 0;
	sh->live_bytes = 0;
	for (uint32_t s = sh->lo; s < sh->hi;) {
		nocopy_seg *g = &nocopy_a->segs[s];
		if (g->state == SEG_FREE || g->state == SEG_CONT) { /* CONT outside any run: a run taken or freed halfway */
			uint32_t gen = g->gen + 1;
			memset(g, 0, sizeof *g);
			g->gen = gen;
			sh->nfree++;
			s++;
			continue;
		}
		uint32_t k = g->run ? g->run : 1;
		k = k < sh->hi - s ? k : sh->hi - s;
		for (uint32_t j = s + 1; j < s + k; j++) {
			seg_set(j, SEG_CONT);
		}
		bool installed = false;
		for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
			installed |= sh->cur[b] == (int32_t) s;
		}
		if (g->state == SEG_OPEN && !installed) {
			seg_set(s, SEG_SEALED);
		}
		g->pending = 0;
		if (g->state != SEG_DEAD) {
			g->live = 0;
			for (char *p = seg_start(s), *end = p + g->used; p < end;) {
				nocopy_entry *e = (nocopy_entry *) p;
				if (e->size < sizeof *e) {
					break;
				}
				e->live = nocopy_slot_of(sh, e) != NULL;
				g->live += e->live ? e->size : 0;
				p += e->size;
			}
			sh->live_bytes += g->live;
		}
		s += k;
	}
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) { /* entries being filled outside the lock stay pending */
		uint32_t w = __atomic_load_n(&nocopy_a->workers[i].wip, __ATOMIC_RELAXED);
		if (!w || seg_shard(nocopy_seg_of(wip_entry(w))) != sh) {
			continue;
		}
		if (__atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED) == dead) {
			__atomic_store_n(&nocopy_a->workers[i].wip, 0, __ATOMIC_RELAXED); /* the holder itself, between relock and commit */
		} else {
			nocopy_a->segs[nocopy_seg_of(wip_entry(w))].pending++;
		}
	}
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if (nocopy_a->segs[s].state == SEG_DEAD) {
			sh->ndead++;
		} else {
			seg_check(sh, s);
		}
	}
	sh->exp_off = 0;
	sh->exp_idle = 0;
	reclaim(sh);
}

/* sh locked. Expire, reclaim, merge until `total` bytes fit, or give up. */
nocopy_entry *nocopy_alloc(nocopy_shard *sh, size_t total, time_t exp, time_t now)
{
	if (total > (size_t) (sh->hi - sh->lo - 1) << nocopy_seg_shift) {
		return NULL; /* bigger than the shard's slice */
	}
	int b = nocopy_ttl_bucket(exp ? exp - now : 0);
	if (sh->exp_idle != now) { /* the common case: nothing left to expire this second */
		nocopy_expire_step(sh, now, 64);
	}
	nocopy_entry *e = nocopy_seg_alloc(sh, total, false, b, exp);
	if (!e) {
		reclaim(sh);
		e = nocopy_seg_alloc(sh, total, false, b, exp);
	}
	while (!e && nocopy_expire_step(sh, now, 4096)) { /* under pressure: catch up on expiry before copying live data */
		reclaim(sh);
		e = nocopy_seg_alloc(sh, total, false, b, exp);
	}
	if (!e && sh->wip_checked != now) {
		sh->wip_checked = now;
		wip_reap(sh); /* segments held back by dead writers' unfinished entries */
		reclaim(sh);
		e = nocopy_seg_alloc(sh, total, false, b, exp);
	}
	for (uint32_t tries = 2 * (sh->hi - sh->lo); !e && tries && nocopy_merge(sh, false); tries--) {
		reclaim(sh);
		e = nocopy_seg_alloc(sh, total, false, b, exp);
	}
	return e;
}

nocopy_entry *nocopy_lookup(nocopy_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	nocopy_entry *e;
	locate(sh, h, k, klen, &e);
	return e;
}

/* Publish e under its key; add_only fails if a live entry exists. Consumes e either way. Shard stays locked. */
nocopy_put_result nocopy_commit(nocopy_shard *sh, nocopy_entry *e, bool add_only)
{
	nocopy_entry *old;
	nocopy_slot *sp = locate(sh, e->h, nocopy_key(e), e->klen, &old);
	if (add_only && old && !nocopy_expired(old)) {
		nocopy_abort(sh, e);
		return PUT_EXISTS;
	}
	if (!old && sh->nlive >= sh->cap) { /* table full: evict whole segments' worth of entries */
		for (uint32_t tries = 2 * (sh->hi - sh->lo); sh->nlive >= sh->cap && tries && nocopy_merge(sh, true); tries--) {}
		sp = locate(sh, e->h, nocopy_key(e), e->klen, &old);
		if (!old && sh->nlive >= sh->cap) {
			nocopy_abort(sh, e);
			return PUT_FULL;
		}
	}
	if (!old && *ghost_cell(sh, e->h) == ghost_tag(e->h)) {
		*ghost_cell(sh, e->h) = 0;
		e->hot = 2; /* came back after an eviction: survives two merges without a read */
		sh->ghost_hits++;
	}
	e->seq = sh->last_seq = e->seq > sh->last_seq ? e->seq : sh->last_seq + 1;
	sh->inserts++;
	sh->nlive += !old;
	entry_birth(sh, e);
	__atomic_store_n(sp, slot_make(e), __ATOMIC_RELEASE);
	if (old) {
		entry_die(sh, old);
	}
	if (sh->hint) {
		reclaim(sh);
	}
	return PUT_OK;
}

nocopy_entry *nocopy_put_begin(nocopy_put *p, nocopy_ctx *c, uint64_t h, uint32_t klen, uint8_t koff, size_t size, time_t ttl)
{
	if (!c->pin_refs) {
		nocopy_drop_transient(c); /* our own idle pins would block reclaim; only mixed read/write loops pay */
	}
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts); /* inside the lock: measured faster under write contention than before it */
	time_t now = ts.tv_sec, exp = ttl ? now + ttl : 0;
	nocopy_entry *e = nocopy_alloc(sh, size, exp, now);
	if (!e) {
		nocopy_unlock(sh);
		return NULL;
	}
	e->h = h;
	e->klen = klen;
	e->koff = koff;
	e->expires = exp;
	e->created = (uint32_t) now;
	e->flags = 0;
	e->user = 0;
	e->seq = (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec; /* commit makes it unique */
	*p = (nocopy_put) {.c = c, .sh = sh, .e = e, .locked = size <= NOCOPY_INLINE_PUT};
	if (!p->locked) {
		wip_set(c, e);
		nocopy_unlock(sh);
	}
	return e;
}

nocopy_put_result nocopy_put_commit(nocopy_put *p, bool add_only)
{
	if (!p->locked) {
		nocopy_lock(p->sh);
	}
	nocopy_put_result r = nocopy_commit(p->sh, p->e, add_only);
	if (!p->locked) {
		wip_set(p->c, NULL);
	}
	nocopy_unlock(p->sh);
	return r;
}

bool nocopy_remove(nocopy_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	nocopy_entry *old;
	nocopy_slot *sp = locate(sh, h, k, klen, &old);
	if (!old) {
		return false;
	}
	bool found = !nocopy_expired(old);
	slot_remove(sh, sp);
	nocopy_entry_die(sh, old);
	if (sh->hint) {
		reclaim(sh);
	}
	return found;
}

void nocopy_clear(void)
{
	for (uint32_t s = 0; s < nocopy_a->nshards; s++) { /* in order: two clears never deadlock */
		nocopy_lock(&nocopy_a->shards[s]);
	}
	for (uint32_t s = 0; s < nocopy_a->nshards; s++) {
		nocopy_shard *sh = &nocopy_a->shards[s];
		nocopy_slot *t = tab_of(sh);
		for (uint32_t i = 0; i <= nocopy_tab_mask; i++) {
			nocopy_slot sl = t[i];
			__atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
			if (sl) {
				nocopy_entry_die(sh, nocopy_slot_entry(sl));
			}
		}
		sh->nlive = 0;
	}
	__atomic_add_fetch(&nocopy_a->expunges, 1, __ATOMIC_RELAXED);
	for (uint32_t s = 0; s < nocopy_a->nshards; s++) {
		wip_reap(&nocopy_a->shards[s]);
		reclaim(&nocopy_a->shards[s]);
		nocopy_unlock(&nocopy_a->shards[s]);
	}
}

/* ---- scans ---- */

nocopy_entry *nocopy_scan_next(nocopy_ctx *c, uint32_t *sh, uint32_t *i)
{
	for (; *sh < nocopy_a->nshards; (*sh)++, *i = 0) {
		const nocopy_slot *t = nocopy_tabs[*sh];
		while (*i <= nocopy_tab_mask) {
			const nocopy_slot *p = &t[(*i)++];
			nocopy_slot s = __atomic_load_n(p, __ATOMIC_ACQUIRE);
			if (!s) {
				continue;
			}
			nocopy_entry *e = nocopy_slot_entry(s);
			if (nocopy_hold(c, e) && __atomic_load_n(p, __ATOMIC_ACQUIRE) != s) {
				(*i)--; /* re-read this slot */
				continue;
			}
			if (!nocopy_expired(e)) {
				return e;
			}
		}
	}
	return NULL;
}

/* ---- stats ---- */

void nocopy_stats(nocopy_stats_t *st)
{
	memset(st, 0, sizeof *st);
	time_t now = time(NULL);
	for (uint32_t i = 0; i < nocopy_a->nshards; i++) { /* lock-free: a snapshot that may be off by concurrent writes */
		nocopy_shard *sh = &nocopy_a->shards[i];
		st->used += __atomic_load_n(&sh->live_bytes, __ATOMIC_RELAXED);
		st->entries += __atomic_load_n(&sh->nlive, __ATOMIC_RELAXED);
		st->evacuated += __atomic_load_n(&sh->evacuated, __ATOMIC_RELAXED);
		st->evicted += __atomic_load_n(&sh->evicted, __ATOMIC_RELAXED);
		st->expired += __atomic_load_n(&sh->expired, __ATOMIC_RELAXED);
		st->ghost_hits += __atomic_load_n(&sh->ghost_hits, __ATOMIC_RELAXED);
		st->free_segments += __atomic_load_n(&sh->nfree, __ATOMIC_RELAXED);
		st->dead_segments += __atomic_load_n(&sh->ndead, __ATOMIC_RELAXED);
		for (int l = 0; l < NOCOPY_LEASES; l++) {
			st->leases += __atomic_load_n(&sh->leases[l].owner, __ATOMIC_RELAXED) && sh->leases[l].until >= now;
		}
	}
	uint64_t held[NOCOPY_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		for (int w = 0; w < NOCOPY_SEG_WORDS; w++) {
			held[w] |= __atomic_load_n(&nocopy_a->workers[i].bits[w], __ATOMIC_SEQ_CST);
		}
		st->hits += __atomic_load_n(&nocopy_a->workers[i].hits, __ATOMIC_RELAXED);
		st->misses += __atomic_load_n(&nocopy_a->workers[i].misses, __ATOMIC_RELAXED);
	}
	for (int w = 0; w < NOCOPY_SEG_WORDS; w++) {
		st->pinned_segments += (uint32_t) __builtin_popcountll(held[w]);
	}
	st->segments = nocopy_a->nsegs;
	st->segment_size = nocopy_seg_size;
	for (uint32_t i = 0; i < nocopy_a->nshards; i++) {
		st->inserts += nocopy_a->shards[i].inserts; /* racy read, like the other counters */
	}
	st->start_time = nocopy_a->start_time;
	st->shards = nocopy_a->nshards;
	st->slots = (uint64_t) nocopy_a->nshards * (nocopy_tab_mask + 1);
	st->expunges = __atomic_load_n(&nocopy_a->expunges, __ATOMIC_RELAXED);
	st->lock_steals = __atomic_load_n(&nocopy_a->lock_steals, __ATOMIC_RELAXED);
}

uint32_t nocopy_free_runs(uint32_t shard, size_t *off, size_t *len, uint32_t max)
{
	const nocopy_shard *sh = &nocopy_a->shards[shard];
	uint32_t runs = 0;
	for (uint32_t i = sh->lo, start = 0, n = 0; i <= sh->hi && runs < max; i++) { /* adjacent free segments form one block */
		uint8_t st = i < sh->hi ? __atomic_load_n(&nocopy_a->segs[i].state, __ATOMIC_RELAXED) : SEG_OPEN;
		bool free_seg = st == SEG_FREE || st == SEG_DEAD;
		if (free_seg && !n++) {
			start = i;
		} else if (!free_seg && n) {
			off[runs] = (size_t) (start - sh->lo) << nocopy_seg_shift;
			len[runs++] = (size_t) n << nocopy_seg_shift;
			n = 0;
		}
	}
	return runs;
}

/* ---- setup ---- */

const char NOCOPY_TOO_SMALL[] = "shm_size is too small or entries_hint too large";

const char *nocopy_arena_create(size_t size, uint64_t hint, size_t *need)
{
	/* shards only where each slice stays >= 8 MB (an entry must fit in one) and holds >= 16 segments */
	unsigned shift = 16;
	while ((size >> shift) > NOCOPY_MAX_SEGS) {
		shift++;
	}
	uint32_t nshards = 1;
	while (nshards < NOCOPY_MAX_SHARDS && size / (nshards * 2) >= ((size_t) 8 << 20) && (size >> shift) / (nshards * 2) >= 16) {
		nshards *= 2;
	}
	/* ~2 slots per hinted entry, and at least one per 256 bytes of arena (3% of it); fixed for the arena's life */
	uint64_t per = 1024;
	while (per * nshards < hint * 2 || per * nshards < size / 256) {
		per <<= 1;
	}
	size_t header = NOCOPY_ALIGN(sizeof(nocopy_arena)) + (size_t) nshards * per * (sizeof(nocopy_slot) + sizeof(uint32_t));
	header = (header + 63) & ~(size_t) 63;
	*need = header + ((size_t) 4 << shift);
	if (per > ((uint64_t) 1 << 31) || size < *need) {
		return NOCOPY_TOO_SMALL;
	}
	if (size > ((size_t) 32 << 30)) {
		return "larger than 32G";
	}
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		return "mmap failed";
	}
	nocopy_a = p; /* zero-filled by the kernel */
	nocopy_a->size = size;
	nocopy_a->header_size = header;
	nocopy_seg_shift = shift;
	nocopy_seg_size = (size_t) 1 << shift;
	nocopy_seg_base = (char *) nocopy_a + header;
	nocopy_a->nsegs = (uint32_t) ((size - header) >> shift);
	nocopy_a->nsegs -= nocopy_a->nsegs % nshards; /* equal slices */
	nocopy_a->nshards = nshards;
	nocopy_shard_mask = nshards - 1;
	nocopy_tab_mask = (uint32_t) per - 1;
	uint32_t slice = nocopy_a->nsegs / nshards;
	for (uint32_t s = 0; s < nshards; s++) {
		nocopy_shard *sh = &nocopy_a->shards[s];
		nocopy_tabs[s] = (nocopy_slot *) (nocopy_a->tables + (size_t) s * per * sizeof(nocopy_slot));
		nocopy_ghosts[s] = (uint32_t *) (nocopy_a->tables + (size_t) nshards * per * sizeof(nocopy_slot)) + (size_t) s * per;
		sh->idx = s;
		sh->lo = s * slice;
		sh->hi = sh->lo + slice;
		sh->nfree = slice;
		sh->exp_next = sh->lo;
		sh->cap = (uint32_t) (per / 4 * 3);
		for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
			sh->cur[b] = -1;
		}
	}
	nocopy_a->start_time = time(NULL);
	nocopy_self = nocopy_owner_self();
	return NULL;
}

void nocopy_arena_destroy(void)
{
	if (nocopy_a) {
		munmap(nocopy_a, nocopy_a->size);
		nocopy_a = NULL;
	}
}
