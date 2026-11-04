/* stash arena: segments, shards, slot tables, pins, writes and repair. No PHP in here. */

#include "stash_arena_int.h"
#include <sched.h>
#include <stdlib.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

stash_arena *stash_a;
char *stash_seg_base;
unsigned stash_seg_shift;
uint32_t stash_shard_mask, stash_tab_mask;
stash_slot *stash_tabs[STASH_MAX_SHARDS];
uint32_t *stash_ghosts[STASH_MAX_SHARDS];
uint64_t stash_self;
size_t stash_seg_size;

static time_t checked[STASH_MAX_WORKERS]; /* last liveness check of a pinning worker: at most one a second */

/* ---- locks ---- */

static void shard_repair(stash_shard *sh, uint64_t dead);
static void wip_abort(int slot);

/* Slow path of stash_lock(): test-and-test-and-set holding the owner id; yields once the wait gets long, and takes the lock over from a dead holder.
 * Whatever that holder was in the middle of is then repaired (shard_repair).
 * Robust pthread mutexes were measured 2-8x slower under contention (futex sleeps); this keeps the spin. */
void stash_lock_wait(stash_shard *sh)
{
	uint64_t *l = &sh->lock;
	for (unsigned spins = 0;;) {
		uint64_t h = __atomic_load_n(l, __ATOMIC_RELAXED);
		if (!h) {
			if (__atomic_compare_exchange_n(l, &h, stash_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				return;
			}
		} else if (++spins < 128) {
			stash_cpu_relax();
		} else {
			sched_yield();
			if (!(spins & 1023) && h != stash_self && stash_owner_dead(h)
			 && __atomic_compare_exchange_n(l, &h, stash_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				__atomic_add_fetch(&stash_a->lock_steals, 1, __ATOMIC_RELAXED);
				shard_repair(sh, h);
				return;
			}
		}
	}
}

/* ---- workers and pins ---- */

static void bits_clear(int slot)
{
	for (int w = 0; w < STASH_SEG_WORDS; w++) {
		__atomic_store_n(&stash_a->workers[slot].bits[w], 0, __ATOMIC_SEQ_CST);
	}
}

bool stash_claim(stash_ctx *c)
{
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < STASH_MAX_WORKERS; i++) {
			uint64_t o = __atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED);
			if (pass ? !o || !stash_owner_dead(o) : o != 0) {
				continue;
			}
			if (__atomic_compare_exchange_n(&stash_a->workers[i].owner, &o, stash_self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				bits_clear(i);
				wip_abort(i); /* its previous owner died filling an entry */
				uint32_t cur = __atomic_load_n(&stash_a->nworkers, __ATOMIC_RELAXED);
				while (cur < (uint32_t) i + 1
				 && !__atomic_compare_exchange_n(&stash_a->nworkers, &cur, i + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
				c->slot = i;
				c->rng = ((uint32_t) (stash_self ^ (stash_self >> 32)) * 2654435761u + (uint32_t) i) | 1;
				return true;
			}
		}
	}
	c->slot = -1;
	return false;
}

void stash_release(stash_ctx *c)
{
	if (c->slot >= 0) {
		bits_clear(c->slot);
		__atomic_store_n(&stash_a->workers[c->slot].owner, 0, __ATOMIC_SEQ_CST);
		c->slot = -1;
	}
}

/* A forked child gets its own owner id and worker slot, and inherits the parent's pins. */
void stash_after_fork(stash_ctx *c)
{
	stash_self = stash_owner_self();
	if (stash_claim(c)) {
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			__atomic_store_n(&stash_a->workers[c->slot].bits[w], c->mine[w], __ATOMIC_SEQ_CST);
		}
	}
}

/* Drop the kept pins: the caller promises no earlier zero-copy result is used again. */
void stash_keep_release(stash_ctx *c)
{
	memset(c->keep, 0, sizeof c->keep);
	if (!c->pin_refs) {
		c->transient = true;
		stash_drop_transient(c);
	}
}

/* ---- segments (shard locked) ---- */

static void seg_set(uint32_t s, uint8_t st) { __atomic_store_n(&stash_a->segs[s].state, st, __ATOMIC_RELEASE); }

static void seg_check(stash_shard *sh, uint32_t s)
{
	stash_seg *g = &stash_a->segs[s];
	if (g->state == SEG_SEALED && !g->live && !g->pending) {
		seg_set(s, SEG_DEAD);
		sh->ndead++;
		sh->hint = true;
	}
}

static void seg_free(stash_shard *sh, uint32_t s)
{
	uint32_t k = stash_a->segs[s].run;
#ifdef STASH_POISON
	memset(seg_start(s), 0x5A, (size_t) k << stash_seg_shift); /* make use-after-free loud */
#endif
	for (uint32_t j = s; j < s + k; j++) {
		uint32_t gen = stash_a->segs[j].gen + 1;
		memset(&stash_a->segs[j], 0, sizeof(stash_seg)); /* SEG_FREE */
		stash_a->segs[j].gen = gen;
	}
	sh->nfree += k;
	sh->ndead--;
}

/* Hand back sh's dead segments that no live worker pins. */
static void reclaim(stash_shard *sh)
{
	sh->hint = false;
	if (!sh->ndead) {
		return;
	}
	/* Dead set first, pins second: a reader pins, then re-checks its slot; a segment that died after we read the pins
	 * could have been pinned (and re-checked) just before. DEAD only ends here, under this lock. */
	uint64_t dead[STASH_SEG_WORDS] = {0};
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if (stash_a->segs[s].state == SEG_DEAD) {
			dead[s >> 6] |= (uint64_t) 1 << (s & 63);
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	uint64_t held[STASH_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	time_t now = time(NULL);
	for (uint32_t i = 0; i < nw; i++) {
		uint64_t b[STASH_SEG_WORDS], blocks = 0;
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			b[w] = __atomic_load_n(&stash_a->workers[i].bits[w], __ATOMIC_SEQ_CST);
			blocks |= b[w] & dead[w];
		}
		if (!blocks) {
			continue;
		}
		uint64_t o = __atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o && o != stash_self && __atomic_exchange_n(&checked[i], now, __ATOMIC_RELAXED) != now) {
			if (stash_owner_dead(o)) {
				bits_clear(i); /* died holding pins */
				continue;
			}
		}
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
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
		__atomic_add_fetch(&stash_a->unpin_gen, 1, __ATOMIC_RELAXED); /* ask readers to drop idle pins */
	}
}

/* Inline here (the write path), exported for stash_evict.c. */
static inline __attribute__((always_inline)) void entry_die(stash_shard *sh, stash_entry *e)
{
	uint32_t s = stash_seg_of(e);
	e->live = false;
	stash_a->segs[s].live -= e->size;
	sh->live_bytes -= e->size;
	seg_check(sh, s);
}

static inline __attribute__((always_inline)) void entry_birth(stash_shard *sh, stash_entry *e)
{
	uint32_t s = stash_seg_of(e);
	e->live = true;
	stash_a->segs[s].pending--;
	stash_a->segs[s].live += e->size;
	sh->live_bytes += e->size;
}

void stash_entry_die(stash_shard *sh, stash_entry *e) { entry_die(sh, e); }
void stash_entry_birth(stash_shard *sh, stash_entry *e) { entry_birth(sh, e); }

void stash_abort(stash_shard *sh, stash_entry *e)
{
	uint32_t s = stash_seg_of(e);
	stash_a->segs[s].pending--;
	seg_check(sh, s);
}

int stash_ttl_bucket(time_t ttl)
{
	int b = ttl ? 1 : 0;
	while (b && b < STASH_TTL_BUCKETS - 1 && ttl >= ((time_t) 1 << (3 * b))) {
		b++;
	}
	return b;
}

static inline __attribute__((always_inline)) stash_entry *seg_carve(uint32_t s, size_t total, time_t exp)
{
	stash_seg *g = &stash_a->segs[s];
	stash_entry *e = (stash_entry *) (seg_start(s) + g->used);
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
static uint32_t take_run(stash_shard *sh, uint32_t k, uint32_t spare, uint8_t state)
{
	if (sh->nfree < k + spare) {
		return UINT32_MAX;
	}
	for (uint32_t s = sh->lo, n = 0; s < sh->hi; s++) {
		n = stash_a->segs[s].state == SEG_FREE ? n + 1 : 0;
		if (n == k) {
			uint32_t got = s + 1 - k;
			for (uint32_t j = got + 1; j <= s; j++) {
				seg_set(j, SEG_CONT);
			}
			stash_a->segs[got].run = k;
			stash_a->segs[got].born = sh->last_seq;
			seg_set(got, state);
			sh->nfree -= k;
			return got;
		}
	}
	return UINT32_MAX;
}

stash_entry *stash_seg_alloc(stash_shard *sh, size_t total, bool reserve, int b, time_t exp)
{
	uint32_t spare = reserve ? 0 : 1, got;
	if (total > stash_seg_size / 2) { /* a run of contiguous segments */
		got = take_run(sh, (uint32_t) ((total + stash_seg_size - 1) >> stash_seg_shift), spare, SEG_SEALED);
		return got == UINT32_MAX ? NULL : seg_carve(got, total, exp);
	}
	int32_t cur = sh->cur[b];
	if (cur >= 0 && stash_a->segs[cur].used + total <= stash_seg_size) {
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
	stash_a->segs[got].bucket = (uint8_t) b;
	sh->cur[b] = (int32_t) got;
	return seg_carve(got, total, exp);
}

/* ---- slot table (shard locked) ---- */

static inline __attribute__((always_inline)) stash_slot *locate(stash_shard *sh, uint64_t h, const char *k, uint32_t klen, stash_entry **found)
{
	stash_slot *t = tab_of(sh);
	uint32_t tag = (uint32_t) h;
	*found = NULL;
	for (uint32_t i = h & stash_tab_mask;; i = (i + 1) & stash_tab_mask) { /* never full: nlive <= cap < slots */
		stash_slot s = t[i];
		if (!s) {
			return &t[i];
		}
		if ((uint32_t) (s >> 32) == tag) {
			stash_entry *e = stash_slot_entry(s);
			if (e->h == h && e->klen == klen && stash_key_eq(stash_key(e), k, klen)) {
				*found = e;
				return &t[i];
			}
		}
	}
}

/* Backward-shift deletion: no tombstones. Readers that miss while a key moves back past them retry (mseq). */
static void slot_remove(stash_shard *sh, stash_slot *p)
{
	stash_slot *t = tab_of(sh);
	uint32_t m = stash_tab_mask, i = (uint32_t) (p - t);
	bool shifting = false;
	for (uint32_t j = (i + 1) & m;; j = (j + 1) & m) {
		stash_slot s = t[j];
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

stash_slot *stash_slot_of(stash_shard *sh, const stash_entry *e)
{
	stash_slot *t = tab_of(sh);
	for (uint32_t i = e->h & stash_tab_mask;; i = (i + 1) & stash_tab_mask) {
		if (!t[i]) {
			return NULL;
		}
		if (stash_slot_entry(t[i]) == e) {
			return &t[i];
		}
	}
}

void stash_entry_unlink(stash_shard *sh, stash_entry *e)
{
	stash_slot *p = stash_slot_of(sh, e);
	if (p) {
		slot_remove(sh, p);
	}
	stash_entry_die(sh, e);
}

/* ---- dead writers ---- */

/* Before unlocking to fill e outside the lock: record it, so the entry of a writer that dies meanwhile is rolled back. NULL after the commit. */
static void wip_set(stash_ctx *c, const stash_entry *e)
{
	__atomic_store_n(&stash_a->workers[c->slot].wip, e ? (uint32_t) (((const char *) e - (const char *) stash_a) >> 3) : 0, __ATOMIC_RELAXED);
}

static stash_entry *wip_entry(uint32_t w) { return (stash_entry *) ((char *) stash_a + ((size_t) w << 3)); }

static stash_shard *seg_shard(uint32_t s)
{
	for (uint32_t i = 0;; i++) {
		if (s < stash_a->shards[i].hi) {
			return &stash_a->shards[i];
		}
	}
}

/* sh locked. Aborts the unfinished entries in sh of writers that died outside the lock. */
static void wip_reap(stash_shard *sh)
{
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		uint32_t w = __atomic_load_n(&stash_a->workers[i].wip, __ATOMIC_RELAXED);
		if (!w || seg_shard(stash_seg_of(wip_entry(w))) != sh) {
			continue;
		}
		uint64_t o = __atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o != stash_self && (!o || stash_owner_dead(o))) {
			__atomic_store_n(&stash_a->workers[i].wip, 0, __ATOMIC_RELAXED);
			stash_abort(sh, wip_entry(w));
		}
	}
}

/* A worker slot just claimed from a dead owner: abort the entry it was filling. No lock held. */
static void wip_abort(int slot)
{
	uint32_t w = __atomic_load_n(&stash_a->workers[slot].wip, __ATOMIC_RELAXED);
	if (!w) {
		return;
	}
	stash_shard *sh = seg_shard(stash_seg_of(wip_entry(w)));
	stash_lock(sh);
	if (__atomic_load_n(&stash_a->workers[slot].wip, __ATOMIC_RELAXED) == w) { /* not reaped meanwhile */
		__atomic_store_n(&stash_a->workers[slot].wip, 0, __ATOMIC_RELAXED);
		stash_abort(sh, wip_entry(w));
	}
	stash_unlock(sh);
}

/* sh's lock was just taken from a dead holder. Rebuilds what it may have left half done from the slot table, which only
 * ever changes by single stores: duplicate slots and an odd mseq (a delete's shift), segment states (a run taken or freed
 * halfway, an open segment not yet installed), live/pending counts (an entry allocated, moved or unlinked halfway). */
static void shard_repair(stash_shard *sh, uint64_t dead)
{
	stash_slot *t = tab_of(sh);
	if (sh->mseq & 1) {
		__atomic_store_n(&sh->mseq, sh->mseq + 1, __ATOMIC_RELEASE); /* else readers retry every miss forever */
	}
	for (uint32_t i = 0; i <= stash_tab_mask; i++) {
		while (t[i] && stash_slot_of(sh, stash_slot_entry(t[i])) != &t[i]) {
			slot_remove(sh, &t[i]); /* a key the shift copied back but did not yet clear */
		}
	}
	sh->nlive = 0;
	for (uint32_t i = 0; i <= stash_tab_mask; i++) {
		sh->nlive += t[i] != 0;
	}
	for (int b = 0; b < STASH_TTL_BUCKETS; b++) {
		int32_t c = sh->cur[b];
		if (c >= 0 && ((uint32_t) c < sh->lo || (uint32_t) c >= sh->hi || stash_a->segs[c].state != SEG_OPEN)) {
			sh->cur[b] = -1;
		}
	}
	sh->nfree = sh->ndead = 0;
	sh->live_bytes = 0;
	for (uint32_t s = sh->lo; s < sh->hi;) {
		stash_seg *g = &stash_a->segs[s];
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
		for (int b = 0; b < STASH_TTL_BUCKETS; b++) {
			installed |= sh->cur[b] == (int32_t) s;
		}
		if (g->state == SEG_OPEN && !installed) {
			seg_set(s, SEG_SEALED);
		}
		g->pending = 0;
		if (g->state != SEG_DEAD) {
			g->live = 0;
			for (char *p = seg_start(s), *end = p + g->used; p < end;) {
				stash_entry *e = (stash_entry *) p;
				if (e->size < sizeof *e) {
					break;
				}
				e->live = stash_slot_of(sh, e) != NULL;
				g->live += e->live ? e->size : 0;
				p += e->size;
			}
			sh->live_bytes += g->live;
		}
		s += k;
	}
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) { /* entries being filled outside the lock stay pending */
		uint32_t w = __atomic_load_n(&stash_a->workers[i].wip, __ATOMIC_RELAXED);
		if (!w || seg_shard(stash_seg_of(wip_entry(w))) != sh) {
			continue;
		}
		if (__atomic_load_n(&stash_a->workers[i].owner, __ATOMIC_RELAXED) == dead) {
			__atomic_store_n(&stash_a->workers[i].wip, 0, __ATOMIC_RELAXED); /* the holder itself, between relock and commit */
		} else {
			stash_a->segs[stash_seg_of(wip_entry(w))].pending++;
		}
	}
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		if (stash_a->segs[s].state == SEG_DEAD) {
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
stash_entry *stash_alloc(stash_shard *sh, size_t total, time_t exp, time_t now)
{
	if (total > (size_t) (sh->hi - sh->lo - 1) << stash_seg_shift) {
		return NULL; /* bigger than the shard's slice */
	}
	int b = stash_ttl_bucket(exp ? exp - now : 0);
	if (sh->exp_idle != now) { /* the common case: nothing left to expire this second */
		stash_expire_step(sh, now, 64);
	}
	stash_entry *e = stash_seg_alloc(sh, total, false, b, exp);
	if (!e) {
		reclaim(sh);
		e = stash_seg_alloc(sh, total, false, b, exp);
	}
	while (!e && stash_expire_step(sh, now, 4096)) { /* under pressure: catch up on expiry before copying live data */
		reclaim(sh);
		e = stash_seg_alloc(sh, total, false, b, exp);
	}
	if (!e && sh->wip_checked != now) {
		sh->wip_checked = now;
		wip_reap(sh); /* segments held back by dead writers' unfinished entries */
		reclaim(sh);
		e = stash_seg_alloc(sh, total, false, b, exp);
	}
	for (uint32_t tries = 2 * (sh->hi - sh->lo); !e && tries && stash_merge(sh, false); tries--) {
		reclaim(sh);
		e = stash_seg_alloc(sh, total, false, b, exp);
	}
	return e;
}

stash_entry *stash_lookup(stash_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	stash_entry *e;
	locate(sh, h, k, klen, &e);
	return e;
}

/* Publish e under its key; add_only fails if a live entry exists. Consumes e either way. Shard stays locked. */
stash_put_result stash_commit(stash_shard *sh, stash_entry *e, bool add_only)
{
	stash_entry *old;
	stash_slot *sp = locate(sh, e->h, stash_key(e), e->klen, &old);
	if (add_only && old && !stash_expired(old)) {
		stash_abort(sh, e);
		return PUT_EXISTS;
	}
	if (!old && sh->nlive >= sh->cap) { /* table full: evict whole segments' worth of entries */
		for (uint32_t tries = 2 * (sh->hi - sh->lo); sh->nlive >= sh->cap && tries && stash_merge(sh, true); tries--) {}
		sp = locate(sh, e->h, stash_key(e), e->klen, &old);
		if (!old && sh->nlive >= sh->cap) {
			stash_abort(sh, e);
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

stash_entry *stash_put_begin(stash_put *p, stash_ctx *c, uint64_t h, uint32_t klen, uint8_t koff, size_t size, time_t ttl)
{
	if (!c->pin_refs) {
		stash_drop_transient(c); /* our own idle pins would block reclaim; only mixed read/write loops pay */
	}
	stash_shard *sh = stash_shard_of(h);
	stash_lock(sh);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts); /* inside the lock: measured faster under write contention than before it */
	time_t now = ts.tv_sec, exp = ttl ? now + ttl : 0;
	stash_entry *e = stash_alloc(sh, size, exp, now);
	if (!e) {
		stash_unlock(sh);
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
	*p = (stash_put) {.c = c, .sh = sh, .e = e, .locked = size <= STASH_INLINE_PUT};
	if (!p->locked) {
		wip_set(c, e);
		stash_unlock(sh);
	}
	return e;
}

stash_put_result stash_put_commit(stash_put *p, bool add_only)
{
	if (!p->locked) {
		stash_lock(p->sh);
	}
	stash_put_result r = stash_commit(p->sh, p->e, add_only);
	if (!p->locked) {
		wip_set(p->c, NULL);
	}
	stash_unlock(p->sh);
	return r;
}

bool stash_remove(stash_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	stash_entry *old;
	stash_slot *sp = locate(sh, h, k, klen, &old);
	if (!old) {
		return false;
	}
	bool found = !stash_expired(old);
	slot_remove(sh, sp);
	stash_entry_die(sh, old);
	if (sh->hint) {
		reclaim(sh);
	}
	return found;
}

void stash_clear(void)
{
	for (uint32_t s = 0; s < stash_a->nshards; s++) { /* in order: two clears never deadlock */
		stash_lock(&stash_a->shards[s]);
	}
	for (uint32_t s = 0; s < stash_a->nshards; s++) {
		stash_shard *sh = &stash_a->shards[s];
		stash_slot *t = tab_of(sh);
		for (uint32_t i = 0; i <= stash_tab_mask; i++) {
			stash_slot sl = t[i];
			__atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
			if (sl) {
				stash_entry_die(sh, stash_slot_entry(sl));
			}
		}
		sh->nlive = 0;
	}
	__atomic_add_fetch(&stash_a->expunges, 1, __ATOMIC_RELAXED);
	for (uint32_t s = 0; s < stash_a->nshards; s++) {
		wip_reap(&stash_a->shards[s]);
		reclaim(&stash_a->shards[s]);
		stash_unlock(&stash_a->shards[s]);
	}
}

/* ---- scans ---- */

stash_entry *stash_scan_next(stash_ctx *c, uint32_t *sh, uint32_t *i)
{
	for (; *sh < stash_a->nshards; (*sh)++, *i = 0) {
		const stash_slot *t = stash_tabs[*sh];
		while (*i <= stash_tab_mask) {
			const stash_slot *p = &t[(*i)++];
			stash_slot s = __atomic_load_n(p, __ATOMIC_ACQUIRE);
			if (!s) {
				continue;
			}
			stash_entry *e = stash_slot_entry(s);
			if (stash_hold(c, e) && __atomic_load_n(p, __ATOMIC_ACQUIRE) != s) {
				(*i)--; /* re-read this slot */
				continue;
			}
			if (!stash_expired(e)) {
				return e;
			}
		}
	}
	return NULL;
}

/* ---- stats ---- */

void stash_stats(stash_stats_t *st)
{
	memset(st, 0, sizeof *st);
	time_t now = time(NULL);
	for (uint32_t i = 0; i < stash_a->nshards; i++) { /* lock-free: a snapshot that may be off by concurrent writes */
		stash_shard *sh = &stash_a->shards[i];
		st->used += __atomic_load_n(&sh->live_bytes, __ATOMIC_RELAXED);
		st->entries += __atomic_load_n(&sh->nlive, __ATOMIC_RELAXED);
		st->evacuated += __atomic_load_n(&sh->evacuated, __ATOMIC_RELAXED);
		st->evicted += __atomic_load_n(&sh->evicted, __ATOMIC_RELAXED);
		st->expired += __atomic_load_n(&sh->expired, __ATOMIC_RELAXED);
		st->ghost_hits += __atomic_load_n(&sh->ghost_hits, __ATOMIC_RELAXED);
		st->free_segments += __atomic_load_n(&sh->nfree, __ATOMIC_RELAXED);
		st->dead_segments += __atomic_load_n(&sh->ndead, __ATOMIC_RELAXED);
		for (int l = 0; l < STASH_LEASES; l++) {
			st->leases += __atomic_load_n(&sh->leases[l].owner, __ATOMIC_RELAXED) && sh->leases[l].until >= now;
		}
	}
	uint64_t held[STASH_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&stash_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		for (int w = 0; w < STASH_SEG_WORDS; w++) {
			held[w] |= __atomic_load_n(&stash_a->workers[i].bits[w], __ATOMIC_SEQ_CST);
		}
		st->hits += __atomic_load_n(&stash_a->workers[i].hits, __ATOMIC_RELAXED);
		st->misses += __atomic_load_n(&stash_a->workers[i].misses, __ATOMIC_RELAXED);
	}
	for (int w = 0; w < STASH_SEG_WORDS; w++) {
		st->pinned_segments += (uint32_t) __builtin_popcountll(held[w]);
	}
	st->segments = stash_a->nsegs;
	st->segment_size = stash_seg_size;
	for (uint32_t i = 0; i < stash_a->nshards; i++) {
		st->inserts += stash_a->shards[i].inserts; /* racy read, like the other counters */
	}
	st->start_time = stash_a->start_time;
	st->shards = stash_a->nshards;
	st->slots = (uint64_t) stash_a->nshards * (stash_tab_mask + 1);
	st->expunges = __atomic_load_n(&stash_a->expunges, __ATOMIC_RELAXED);
	st->lock_steals = __atomic_load_n(&stash_a->lock_steals, __ATOMIC_RELAXED);
}

uint32_t stash_free_runs(uint32_t shard, size_t *off, size_t *len, uint32_t max)
{
	const stash_shard *sh = &stash_a->shards[shard];
	uint32_t runs = 0;
	for (uint32_t i = sh->lo, start = 0, n = 0; i <= sh->hi && runs < max; i++) { /* adjacent free segments form one block */
		uint8_t st = i < sh->hi ? __atomic_load_n(&stash_a->segs[i].state, __ATOMIC_RELAXED) : SEG_OPEN;
		bool free_seg = st == SEG_FREE || st == SEG_DEAD;
		if (free_seg && !n++) {
			start = i;
		} else if (!free_seg && n) {
			off[runs] = (size_t) (start - sh->lo) << stash_seg_shift;
			len[runs++] = (size_t) n << stash_seg_shift;
			n = 0;
		}
	}
	return runs;
}

/* ---- setup ---- */

const char STASH_TOO_SMALL[] = "shm_size is too small or entries_hint too large";

const char *stash_arena_create(size_t size, uint64_t hint, size_t *need)
{
	/* shards only where each slice stays >= 8 MB (an entry must fit in one) and holds >= 16 segments */
	unsigned shift = 16;
	while ((size >> shift) > STASH_MAX_SEGS) {
		shift++;
	}
	uint32_t nshards = 1;
	while (nshards < STASH_MAX_SHARDS && size / (nshards * 2) >= ((size_t) 8 << 20) && (size >> shift) / (nshards * 2) >= 16) {
		nshards *= 2;
	}
	/* ~2 slots per hinted entry, and at least one per 256 bytes of arena (3% of it); fixed for the arena's life */
	uint64_t per = 1024;
	while (per * nshards < hint * 2 || per * nshards < size / 256) {
		per <<= 1;
	}
	size_t header = STASH_ALIGN(sizeof(stash_arena)) + (size_t) nshards * per * (sizeof(stash_slot) + sizeof(uint32_t));
	header = (header + 63) & ~(size_t) 63;
	*need = header + ((size_t) 4 << shift);
	if (per > ((uint64_t) 1 << 31) || size < *need) {
		return STASH_TOO_SMALL;
	}
	if (size > ((size_t) 32 << 30)) {
		return "larger than 32G";
	}
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		return "mmap failed";
	}
	stash_a = p; /* zero-filled by the kernel */
	stash_a->size = size;
	stash_a->header_size = header;
	stash_seg_shift = shift;
	stash_seg_size = (size_t) 1 << shift;
	stash_seg_base = (char *) stash_a + header;
	stash_a->nsegs = (uint32_t) ((size - header) >> shift);
	stash_a->nsegs -= stash_a->nsegs % nshards; /* equal slices */
	stash_a->nshards = nshards;
	stash_shard_mask = nshards - 1;
	stash_tab_mask = (uint32_t) per - 1;
	uint32_t slice = stash_a->nsegs / nshards;
	for (uint32_t s = 0; s < nshards; s++) {
		stash_shard *sh = &stash_a->shards[s];
		stash_tabs[s] = (stash_slot *) (stash_a->tables + (size_t) s * per * sizeof(stash_slot));
		stash_ghosts[s] = (uint32_t *) (stash_a->tables + (size_t) nshards * per * sizeof(stash_slot)) + (size_t) s * per;
		sh->idx = s;
		sh->lo = s * slice;
		sh->hi = sh->lo + slice;
		sh->nfree = slice;
		sh->exp_next = sh->lo;
		sh->cap = (uint32_t) (per / 4 * 3);
		for (int b = 0; b < STASH_TTL_BUCKETS; b++) {
			sh->cur[b] = -1;
		}
	}
	stash_a->start_time = time(NULL);
	stash_self = stash_owner_self();
	return NULL;
}

void stash_arena_destroy(void)
{
	if (stash_a) {
		munmap(stash_a, stash_a->size);
		stash_a = NULL;
	}
}
