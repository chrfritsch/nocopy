/* nocopy arena: segments, shards, slot tables, pins, writes and repair. No PHP in here. */

#include "nocopy_arena_int.h"
#include <sched.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

nocopy_arena *nocopy_a;
char *nocopy_seg_base;
unsigned nocopy_seg_shift;
uint32_t nocopy_seg_words;
nocopy_seg *nocopy_segs;
uint8_t *nocopy_seg_owner;
uint64_t *nocopy_pins;
uint32_t nocopy_pin_stride;
uint32_t nocopy_shard_mask, nocopy_tab_mask;
nocopy_slot *nocopy_tabs[NOCOPY_MAX_SHARDS];
uint32_t *nocopy_ghosts[NOCOPY_MAX_SHARDS];
uint64_t nocopy_self;
size_t nocopy_seg_size;
uint32_t nocopy_fair_segs;

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
	for (uint32_t w = 0; w < nocopy_seg_words; w++) {
		__atomic_store_n(&nocopy_wbits(slot)[w], 0, __ATOMIC_SEQ_CST);
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
				__atomic_store_n(&nocopy_a->workers[i].agen, 0, __ATOMIC_RELAXED); /* ... or holding an atom view */
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
		for (uint32_t w = 0; w < nocopy_seg_words; w++) {
			__atomic_store_n(&nocopy_wbits(c->slot)[w], c->mine[w], __ATOMIC_SEQ_CST);
		}
	}
}

void nocopy_request_end(nocopy_ctx *c)
{
	if (c->anest) { /* a write bailed out between nocopy_atom_begin and _end */
		c->anest = 1;
		nocopy_atom_end(c);
	}
	c->pin_refs = 0;
	memset(c->keep, 0, sizeof c->keep); /* no earlier zero-copy result is used again */
	c->nkeep = 0;
	c->transient = true;
	nocopy_drop_transient(c);
}

/* ---- segments (shard locked) ---- */

static void seg_set(uint32_t s, uint8_t st) { __atomic_store_n(&nocopy_segs[s].state, st, __ATOMIC_RELEASE); }

static void seg_check(nocopy_shard *sh, uint32_t s)
{
	nocopy_seg *g = &nocopy_segs[s];
	if (g->state == SEG_SEALED && !g->live && !g->pending) {
		seg_set(s, SEG_DEAD);
		sh->ndead++;
		sh->hint = true;
	}
}

/* Bucket b's open segment, if any, sealed: no more writes go to it. */
static void seg_seal(nocopy_shard *sh, int b)
{
	int32_t cur = sh->cur[b];
	if (cur >= 0) {
		seg_set((uint32_t) cur, SEG_SEALED);
		seg_check(sh, (uint32_t) cur);
		sh->cur[b] = -1;
	}
}

/* ---- the pool: free segments belong to no shard; owner[] changes only by single atomic stores ---- */

static bool seg_claim(nocopy_shard *sh, uint32_t s)
{
	uint8_t z = 0;
	if (!__atomic_compare_exchange_n(&nocopy_seg_owner[s], &z, (uint8_t) (sh->idx + 1), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return false;
	}
	nocopy_segs_add(sh->own, s);
	__atomic_store_n(&sh->nown, sh->nown + 1, __ATOMIC_RELAXED);
	return true;
}

/* s is FREE and zeroed (but gen). */
static void seg_release(nocopy_shard *sh, uint32_t s)
{
	nocopy_segs_del(sh->own, s);
	__atomic_store_n(&sh->nown, sh->nown - 1, __ATOMIC_RELAXED);
	__atomic_store_n(&nocopy_seg_owner[s], 0, __ATOMIC_RELEASE);
}

static uint32_t pool_free(void)
{
	uint32_t n = 0;
	for (uint32_t s = 0; s < nocopy_a->nsegs; s++) {
		n += !__atomic_load_n(&nocopy_seg_owner[s], __ATOMIC_RELAXED);
	}
	return n;
}

static void seg_zero(uint32_t s)
{
	uint32_t gen = nocopy_segs[s].gen + 1;
	memset(&nocopy_segs[s], 0, sizeof(nocopy_seg)); /* SEG_FREE */
	nocopy_segs[s].gen = gen;
}

static void seg_free(nocopy_shard *sh, uint32_t s)
{
	uint32_t k = nocopy_segs[s].run;
#ifdef NOCOPY_POISON
	memset(seg_start(s), 0x5A, (size_t) k << nocopy_seg_shift); /* make use-after-free loud */
#endif
	for (uint32_t j = s; j < s + k; j++) {
		seg_zero(j);
		seg_release(sh, j);
	}
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
	SH_SEGS(sh, s) {
		if (nocopy_segs[s].state == SEG_DEAD) {
			nocopy_segs_add(dead, s);
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	uint64_t held[NOCOPY_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	time_t now = nocopy_now();
	uint32_t nwords = nocopy_seg_words;
	for (uint32_t i = 0; i < nw; i++) {
		uint64_t b[NOCOPY_SEG_WORDS], blocks = 0;
		for (uint32_t w = 0; w < nwords; w++) {
			b[w] = __atomic_load_n(&nocopy_wbits(i)[w], __ATOMIC_SEQ_CST);
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
		for (uint32_t w = 0; w < nwords; w++) {
			held[w] |= b[w];
		}
	}
	bool blocked = false;
	SH_SEGS(sh, s) {
		if (!nocopy_segs_has(dead, s)) {
			continue;
		}
		if (nocopy_segs_has(held, s)) {
			blocked = true;
		} else {
			seg_free(sh, s);
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
	__atomic_sub_fetch(&nocopy_segs[s].reads, (1 << __atomic_load_n(&e->hot, __ATOMIC_RELAXED)) - 1, __ATOMIC_RELAXED);
	nocopy_segs[s].live -= e->size;
	sh->live_bytes -= e->size;
	seg_check(sh, s);
}

static inline __attribute__((always_inline)) void entry_birth(nocopy_shard *sh, nocopy_entry *e)
{
	uint32_t s = nocopy_seg_of(e);
	e->live = true;
	nocopy_segs[s].pending--;
	__atomic_add_fetch(&nocopy_segs[s].reads, (1 << __atomic_load_n(&e->hot, __ATOMIC_RELAXED)) - 1, __ATOMIC_RELAXED);
	nocopy_segs[s].live += e->size;
	sh->live_bytes += e->size;
}

void nocopy_entry_die(nocopy_shard *sh, nocopy_entry *e) { entry_die(sh, e); }
void nocopy_entry_birth(nocopy_shard *sh, nocopy_entry *e) { entry_birth(sh, e); }

void nocopy_abort(nocopy_shard *sh, nocopy_entry *e)
{
	uint32_t s = nocopy_seg_of(e);
	nocopy_segs[s].pending--;
	seg_check(sh, s);
}

static int ttl_bucket(time_t ttl)
{
	int b = ttl ? 1 : 0;
	while (b && b < NOCOPY_TTL_BUCKETS - 1 && ttl >= ((time_t) 1 << (3 * b))) {
		b++;
	}
	return b;
}

static inline __attribute__((always_inline)) nocopy_entry *seg_carve(uint32_t s, size_t total, time_t exp)
{
	nocopy_seg *g = &nocopy_segs[s];
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

/* k contiguous segments from the pool; the first gets `state`. Unless reserve, one free segment per shard stays, so every merge can move survivors. UINT32_MAX if none. */
static __attribute__((noinline)) uint32_t take_run(nocopy_shard *sh, uint32_t k, bool reserve, uint8_t state)
{
	if (pool_free() < k + (reserve ? 0 : nocopy_a->nshards)) {
		return UINT32_MAX;
	}
	/* from its home range on: balanced shards stay contiguous (interleaved, their nocopy_segs share cache lines: parallel stores -50%) */
	uint32_t ns = nocopy_a->nsegs, home = sh->idx * nocopy_fair_segs;
	for (uint32_t i = 0, n = 0; i < ns; i++) {
		uint32_t s = (home + i) % ns;
		n = __atomic_load_n(&nocopy_seg_owner[s], __ATOMIC_RELAXED) ? 0 : s ? n + 1 : 1; /* runs do not wrap */
		if (n < k) {
			continue;
		}
		uint32_t got = s + 1 - k, j = got;
		while (j <= s && seg_claim(sh, j)) {
			j++;
		}
		if (j <= s) { /* another shard took j first */
			for (uint32_t r = got; r < j; r++) {
				seg_release(sh, r);
			}
			i -= s - j; /* resume after j */
			n = 0;
			continue;
		}
		for (j = got + 1; j <= s; j++) {
			seg_set(j, SEG_CONT);
		}
		nocopy_segs[got].run = k;
		nocopy_segs[got].born = sh->last_seq;
		__atomic_store_n(&nocopy_segs[got].reads, 0, __ATOMIC_RELAXED);
		seg_set(got, state);
		return got;
	}
	return UINT32_MAX;
}

/* A segment (or run) from the pool. Out of line: its registers cost the fast path a longer prologue (+5 instructions a write). */
static __attribute__((noinline)) nocopy_entry *seg_alloc_new(nocopy_shard *sh, size_t total, bool reserve, int b, time_t exp)
{
	uint32_t got;
	if (total > nocopy_seg_size / 2) { /* a run of contiguous segments */
		got = take_run(sh, (uint32_t) ((total + nocopy_seg_size - 1) >> nocopy_seg_shift), reserve, SEG_SEALED);
		return got == UINT32_MAX ? NULL : seg_carve(got, total, exp);
	}
	got = take_run(sh, 1, reserve, SEG_OPEN);
	if (got == UINT32_MAX) {
		return NULL;
	}
	seg_seal(sh, b);
	nocopy_segs[got].bucket = (uint8_t) b;
	sh->cur[b] = (int32_t) got;
	return seg_carve(got, total, exp);
}

nocopy_entry *nocopy_seg_alloc(nocopy_shard *sh, size_t total, bool reserve, int b, time_t exp)
{
	int32_t cur = sh->cur[b];
	if (total <= nocopy_seg_size / 2 && cur >= 0 && nocopy_segs[cur].used + total <= nocopy_seg_size) {
		return seg_carve(cur, total, exp);
	}
	return seg_alloc_new(sh, total, reserve, b, exp);
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
static void wip_clear(uint32_t slot) { __atomic_store_n(&nocopy_a->workers[slot].wip, 0, __ATOMIC_RELAXED); }

/* NULL: in the pool. A wip entry's segment stays with its shard: the entry keeps it pending. */
static nocopy_shard *seg_shard(uint32_t s)
{
	uint8_t o = __atomic_load_n(&nocopy_seg_owner[s], __ATOMIC_ACQUIRE);
	return o ? &nocopy_a->shards[o - 1] : NULL;
}

/* The entry worker slot i is filling in sh, or NULL. */
static nocopy_entry *wip_in(uint32_t i, const nocopy_shard *sh)
{
	uint32_t w = __atomic_load_n(&nocopy_a->workers[i].wip, __ATOMIC_RELAXED);
	return w && seg_shard(nocopy_seg_of(wip_entry(w))) == sh ? wip_entry(w) : NULL;
}

/* sh locked. Aborts the unfinished entries in sh of writers that died outside the lock. */
static void wip_reap(nocopy_shard *sh)
{
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		nocopy_entry *e = wip_in(i, sh);
		uint64_t o = e ? __atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED) : 0;
		if (e && o != nocopy_self && (!o || nocopy_owner_dead(o))) {
			wip_clear(i);
			nocopy_abort(sh, e);
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
	if (!sh) {
		wip_clear((uint32_t) slot);
		return;
	}
	nocopy_lock(sh);
	if (__atomic_load_n(&nocopy_a->workers[slot].wip, __ATOMIC_RELAXED) == w) { /* not reaped meanwhile */
		wip_clear((uint32_t) slot);
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
	memset(sh->own, 0, sizeof sh->own); /* owner[] is the truth: a claim or release may have stopped halfway */
	uint32_t nown = 0;
	for (uint32_t s = 0; s < nocopy_a->nsegs; s++) {
		if (__atomic_load_n(&nocopy_seg_owner[s], __ATOMIC_ACQUIRE) == sh->idx + 1) {
			nocopy_segs_add(sh->own, s);
			nown++;
		}
	}
	__atomic_store_n(&sh->nown, nown, __ATOMIC_RELAXED);
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		int32_t c = sh->cur[b];
		if (c >= 0 && (!owns(sh, (uint32_t) c) || nocopy_segs[c].state != SEG_OPEN)) {
			sh->cur[b] = -1;
		}
	}
	sh->ndead = 0;
	sh->live_bytes = 0;
	for (uint32_t s = own_next(sh, 0); s != UINT32_MAX;) {
		nocopy_seg *g = &nocopy_segs[s];
		if (g->state == SEG_FREE || g->state == SEG_CONT) { /* claimed but not set up, or CONT outside any run: a run taken or freed halfway */
			seg_zero(s);
			seg_release(sh, s);
			s = own_next(sh, s + 1);
			continue;
		}
		uint32_t k = 1;
		while (k < g->run && s + k < nocopy_a->nsegs && owns(sh, s + k)) {
			k++;
		}
		g->run = k; /* seg_free releases run segments: never more than it owns */
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
		s = own_next(sh, s + k);
	}
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) { /* entries being filled outside the lock stay pending */
		nocopy_entry *e = wip_in(i, sh);
		if (!e) {
			continue;
		}
		if (__atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED) == dead) {
			wip_clear(i); /* the holder itself, between relock and commit */
		} else {
			nocopy_segs[nocopy_seg_of(e)].pending++;
		}
	}
	SH_SEGS(sh, s) {
		if (nocopy_segs[s].state == SEG_DEAD) {
			sh->ndead++;
		} else {
			seg_check(sh, s);
		}
	}
	sh->exp_off = 0;
	sh->exp_idle = 0;
	reclaim(sh);
}

/* sh locked, below its share, and the pool is dry: the shard owning the most evicts once to give segments back.
 * Drops sh's lock meanwhile (one lock at a time). false: nobody holds more than its share. */
static __attribute__((cold, noinline)) bool rebalance(nocopy_shard *sh)
{
	uint32_t most = nocopy_fair_segs;
	nocopy_shard *big = NULL;
	for (uint32_t i = 0; i < nocopy_a->nshards; i++) {
		uint32_t n = __atomic_load_n(&nocopy_a->shards[i].nown, __ATOMIC_RELAXED);
		if (n > most) {
			most = n;
			big = &nocopy_a->shards[i];
		}
	}
	if (!big) {
		return false;
	}
	nocopy_unlock(sh);
	nocopy_lock(big);
	reclaim(big);
	if (big->nown > nocopy_fair_segs && nocopy_merge(big, false)) {
		reclaim(big);
	}
	nocopy_unlock(big);
	nocopy_lock(sh);
	return true;
}

static nocopy_entry *reclaim_alloc(nocopy_shard *sh, size_t total, int b, time_t exp)
{
	reclaim(sh);
	return nocopy_seg_alloc(sh, total, false, b, exp);
}

/* sh locked (dropped meanwhile by rebalance). Expire, reclaim, take back its share, merge until `total` bytes fit, or give up. */
nocopy_entry *nocopy_alloc(nocopy_shard *sh, size_t total, time_t exp, time_t now)
{
	if (total > (size_t) (nocopy_fair_segs - 1) << nocopy_seg_shift) {
		return NULL; /* bigger than a shard's share */
	}
	int b = ttl_bucket(exp ? exp - now : 0);
	if (sh->exp_idle != now) { /* the common case: nothing left to expire this second */
		nocopy_expire_step(sh, now, 64);
	}
	nocopy_entry *e = nocopy_seg_alloc(sh, total, false, b, exp);
	if (!e) {
		e = reclaim_alloc(sh, total, b, exp);
	}
	while (!e && nocopy_expire_step(sh, now, 4096)) { /* under pressure: catch up on expiry before copying live data */
		e = reclaim_alloc(sh, total, b, exp);
	}
	if (!e && sh->wip_checked != now) {
		sh->wip_checked = now;
		wip_reap(sh); /* segments held back by dead writers' unfinished entries */
		e = reclaim_alloc(sh, total, b, exp);
	}
	for (uint32_t tries = 2 * nocopy_fair_segs; !e && tries && sh->nown < nocopy_fair_segs - nocopy_fair_segs / 16 && rebalance(sh); tries--) {
		e = nocopy_seg_alloc(sh, total, false, b, exp);
	}
	for (uint32_t tries = 2 * nocopy_fair_segs; !e && tries && nocopy_merge(sh, false); tries--) {
		e = reclaim_alloc(sh, total, b, exp);
	}
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
		for (uint32_t tries = 2 * nocopy_fair_segs; sh->nlive >= sh->cap && tries && nocopy_merge(sh, true); tries--) {}
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
	e->seq = sh->last_seq = e->seq > sh->last_seq ? e->seq : sh->last_seq + (1u << NOCOPY_SEQ_SHARD_BITS); /* keeps the shard bits */
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

nocopy_entry *nocopy_put_begin(nocopy_put *p, nocopy_ctx *c, uint64_t h, uint32_t klen, uint8_t koff, size_t size, time_t ttl, uint8_t atoms)
{
	if (!c->pin_refs) {
		nocopy_drop_transient(c); /* our own idle pins would block reclaim; only mixed read/write loops pay */
	}
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	struct timespec ts;
	nocopy_clock(&ts); /* inside the lock: measured faster under write contention than before it */
	time_t now = ts.tv_sec, exp = nocopy_expiry(now, ttl);
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
	if (atoms) { /* before the writer drops its view: atom_rotate relies on it */
		nocopy_segs[nocopy_seg_of(e)].atoms |= atoms;
	}
	e->seq = ((uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u) << NOCOPY_SEQ_SHARD_BITS | sh->idx; /* commit makes it unique */
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

bool nocopy_delete(uint64_t h, const char *k, uint32_t klen)
{
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	nocopy_entry *old;
	nocopy_slot *sp = locate(sh, h, k, klen, &old);
	bool found = old && !nocopy_expired(old);
	if (old) {
		slot_remove(sh, sp);
		nocopy_entry_die(sh, old);
		if (sh->hint) {
			reclaim(sh);
		}
	}
	nocopy_unlock(sh);
	return found;
}

/* Out of line: locate inlined into nocopy_update made inc 10% slower (ab write). */
static __attribute__((noinline)) nocopy_entry *lookup(nocopy_shard *sh, uint64_t h, const char *k, uint32_t klen)
{
	nocopy_entry *e;
	locate(sh, h, k, klen, &e);
	return e;
}

int nocopy_update(uint64_t h, const char *k, uint32_t klen, int (*fn)(nocopy_entry *e, void *arg), void *arg)
{
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	nocopy_entry *e = lookup(sh, h, k, klen);
	int r = e && !nocopy_expired(e) ? fn(e, arg) : NOCOPY_GONE;
	nocopy_unlock(sh);
	return r;
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
		nocopy_shard *sh = &nocopy_a->shards[s];
		wip_reap(sh);
		for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
			seg_seal(sh, b); /* so they die and go back to the pool (with their atom bits) */
		}
		reclaim(sh);
		nocopy_unlock(sh);
	}
}

void nocopy_each(nocopy_ctx *c, void (*fn)(const nocopy_entry *e, void *arg), void *arg)
{
	time_t now = nocopy_now();
	for (uint32_t si = 0; si < nocopy_a->nshards; si++) {
		const nocopy_slot *t = nocopy_tabs[si];
		for (uint32_t i = 0; i <= nocopy_tab_mask; i++) {
			nocopy_slot s = __atomic_load_n(&t[i], __ATOMIC_ACQUIRE);
			if (!s) {
				continue;
			}
			nocopy_entry *e = nocopy_slot_entry(s);
			if (nocopy_hold(c, e) && __atomic_load_n(&t[i], __ATOMIC_ACQUIRE) != s) {
				continue; /* unlinked while pinning: the memory may be reused */
			}
			if (!e->expires || e->expires >= now) {
				fn(e, arg);
			}
		}
	}
}

/* ---- stats ---- */

void nocopy_stats(nocopy_stats_t *st)
{
	memset(st, 0, sizeof *st);
	time_t now = nocopy_now();
	for (uint32_t i = 0; i < nocopy_a->nshards; i++) { /* lock-free: a snapshot that may be off by concurrent writes */
		nocopy_shard *sh = &nocopy_a->shards[i];
		st->used += __atomic_load_n(&sh->live_bytes, __ATOMIC_RELAXED);
		st->entries += __atomic_load_n(&sh->nlive, __ATOMIC_RELAXED);
		st->evacuated += __atomic_load_n(&sh->evacuated, __ATOMIC_RELAXED);
		st->evicted += __atomic_load_n(&sh->evicted, __ATOMIC_RELAXED);
		st->expired += __atomic_load_n(&sh->expired, __ATOMIC_RELAXED);
		st->ghost_hits += __atomic_load_n(&sh->ghost_hits, __ATOMIC_RELAXED);
		st->dead_segments += __atomic_load_n(&sh->ndead, __ATOMIC_RELAXED);
		for (int l = 0; l < NOCOPY_LEASES; l++) {
			st->leases += __atomic_load_n(&sh->leases[l].owner, __ATOMIC_RELAXED) && sh->leases[l].until >= now;
		}
	}
	uint64_t held[NOCOPY_SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		for (uint32_t w = 0; w < nocopy_seg_words; w++) {
			held[w] |= __atomic_load_n(&nocopy_wbits(i)[w], __ATOMIC_SEQ_CST);
		}
		st->hits += __atomic_load_n(&nocopy_a->workers[i].hits, __ATOMIC_RELAXED);
		st->misses += __atomic_load_n(&nocopy_a->workers[i].misses, __ATOMIC_RELAXED);
	}
	for (uint32_t w = 0; w < nocopy_seg_words; w++) {
		st->pinned_segments += (uint32_t) __builtin_popcountll(held[w]);
	}
	st->segments = nocopy_a->nsegs;
	st->free_segments = pool_free();
	st->segment_size = nocopy_seg_size;
	st->start_time = nocopy_a->start_time;
	st->shards = nocopy_a->nshards;
	st->slots = (uint64_t) nocopy_a->nshards * (nocopy_tab_mask + 1);
	st->expunges = __atomic_load_n(&nocopy_a->expunges, __ATOMIC_RELAXED);
	st->lock_steals = __atomic_load_n(&nocopy_a->lock_steals, __ATOMIC_RELAXED);
	const nocopy_atom_half *cur = &nocopy_a->atom[__atomic_load_n(&nocopy_a->atom_gen, __ATOMIC_RELAXED) & 1];
	st->atoms = __atomic_load_n(&cur->n, __ATOMIC_RELAXED);
	st->atoms_used = __atomic_load_n(&nocopy_a->atom[0].used, __ATOMIC_RELAXED) + __atomic_load_n(&nocopy_a->atom[1].used, __ATOMIC_RELAXED) - 16;
	st->atoms_size = 2 * (uint64_t) nocopy_a->atom_half;
	st->atom_rotations = __atomic_load_n(&nocopy_a->atom_rotations, __ATOMIC_RELAXED);
}

/* ---- setup ---- */

/* Process-local copies of the layout, from the header. */
static void adopt(nocopy_arena *p)
{
	nocopy_a = p;
	nocopy_seg_shift = p->seg_shift;
	nocopy_seg_size = (size_t) 1 << p->seg_shift;
	nocopy_seg_base = (char *) p + p->header_size;
	nocopy_shard_mask = p->nshards - 1;
	nocopy_fair_segs = p->nsegs / p->nshards;
	nocopy_seg_words = (p->nsegs + 63) / 64;
	nocopy_segs = (nocopy_seg *) ((char *) p + p->segs_off);
	nocopy_seg_owner = (uint8_t *) p + p->owner_off;
	nocopy_pins = (uint64_t *) ((char *) p + p->pins_off);
	nocopy_pin_stride = p->pin_stride;
	nocopy_tab_mask = p->tab_per - 1;
	for (uint32_t s = 0; s < p->nshards; s++) {
		nocopy_tabs[s] = (nocopy_slot *) (p->tables + (size_t) s * p->tab_per * sizeof(nocopy_slot));
		nocopy_ghosts[s] = (uint32_t *) (p->tables + (size_t) p->nshards * p->tab_per * sizeof(nocopy_slot)) + (size_t) s * p->tab_per;
	}
	nocopy_atoms = (char *) p + p->atom_off;
	nocopy_atom_tab = (nocopy_slot *) ((char *) p + p->atom_tab_off);
	nocopy_atom_dk = (uint32_t *) ((char *) p + p->atom_dk_off);
	nocopy_atom_mask = p->atom_slots - 1;
	nocopy_atom_bytes = p->atom_half;
	nocopy_self = nocopy_owner_self();
}

typedef struct {
	size_t size, header, segs_off, owner_off, pins_off, atom_off, atom_tab_off, atom_dk_off;
	unsigned shift;
	uint32_t nshards, per, pin_stride, atom_half, atom_slots;
} layout;

const char NOCOPY_TOO_SMALL[] = "shm_size is too small or entries_hint too large";

static const char *plan(layout *l, size_t size, uint64_t hint, size_t *need)
{
	/* shards only where each slice stays >= 8 MB (an entry must fit in one) and holds >= 16 segments */
	unsigned shift = 16;
	while ((size >> shift) > 256 && shift < NOCOPY_SEG_MAX_SHIFT) {
		shift++;
	}
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
	/* after the tables: per-segment state, owners and pin bitmaps, sized for size >> shift segments (an upper bound) */
	size_t maxsegs = size >> shift, a64 = ~(size_t) 63;
	uint32_t stride = (uint32_t) (((maxsegs + 63) / 64 + 7) & ~(size_t) 7);
	size_t segs_off = (NOCOPY_ALIGN(sizeof(nocopy_arena)) + (size_t) nshards * per * (sizeof(nocopy_slot) + sizeof(uint32_t)) + 63) & a64;
	size_t owner_off = (segs_off + maxsegs * sizeof(nocopy_seg) + 63) & a64;
	size_t pins_off = (owner_off + maxsegs + 63) & a64;
	/* atoms: two halves of size/128 each (a warmed Drupal shares 0.3 MB of strings in a 47 MB arena); a slot per 32 bytes, at most 3/4 used */
	uint32_t atom_half = (uint32_t) ((size >> 7) & ~(size_t) 7), atom_slots = 64;
	while (atom_slots < atom_half / 32) {
		atom_slots <<= 1;
	}
	size_t atom_tab_off = (pins_off + (size_t) NOCOPY_MAX_WORKERS * stride * sizeof(uint64_t) + 63) & a64;
	size_t atom_dk_off = atom_tab_off + 2 * (size_t) atom_slots * sizeof(nocopy_slot);
	size_t atom_off = (atom_dk_off + 2 * (size_t) atom_slots * sizeof(uint32_t) + 63) & a64;
	size_t header = (atom_off + 2 * (size_t) atom_half + 63) & a64;
	*need = header + ((size_t) 4 << shift);
	if (per > ((uint64_t) 1 << 31) || size < *need) {
		return NOCOPY_TOO_SMALL;
	}
	if (size > ((size_t) 32 << 30)) {
		return "larger than 32G";
	}
	*l = (layout) {.size = size, .header = header, .segs_off = segs_off, .owner_off = owner_off, .pins_off = pins_off,
		.atom_off = atom_off, .atom_tab_off = atom_tab_off, .atom_dk_off = atom_dk_off,
		.shift = shift, .nshards = nshards, .per = (uint32_t) per, .pin_stride = stride, .atom_half = atom_half, .atom_slots = atom_slots};
	return NULL;
}

/* p is zero-filled. */
static void init(nocopy_arena *p, const layout *l)
{
	p->base = (uintptr_t) p;
	p->size = l->size;
	p->header_size = l->header;
	p->seg_shift = l->shift;
	p->tab_per = l->per;
	p->nsegs = (uint32_t) ((l->size - l->header) >> l->shift);
	p->nshards = l->nshards;
	p->segs_off = l->segs_off;
	p->owner_off = l->owner_off;
	p->pins_off = l->pins_off;
	p->pin_stride = l->pin_stride;
	p->atom_off = l->atom_off;
	p->atom_tab_off = l->atom_tab_off;
	p->atom_dk_off = l->atom_dk_off;
	p->atom_half = l->atom_half;
	p->atom_slots = l->atom_slots;
	nocopy_atom_init(p);
	for (uint32_t s = 0; s < l->nshards; s++) {
		nocopy_shard *sh = &p->shards[s];
		sh->idx = s;
		sh->cap = l->per / 4 * 3;
		for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
			sh->cur[b] = -1;
		}
	}
	p->start_time = nocopy_now();
}

/* ---- named arenas ---- */

#define NOCOPY_MAGIC 0x6e6f636f70790001ULL

/* The object of a named arena, open for the process's life with a shared flock on it (inherited by forks):
 * an exclusive flock succeeds only while no process maps the object. */
static int named_fd = -1;
static char named_name[64];
static size_t mapped_size; /* not the header's: workers may write it */
static struct stat named_st; /* the listener's, see nocopy_arena_unhold */

static bool same_file(const struct stat *a, const struct stat *b) { return a->st_dev == b->st_dev && a->st_ino == b->st_ino; }

/* Ours alone: another user's object would hand us its pointers (root ignores the mode). */
static bool owned(int fd)
{
	struct stat st;
	return fstat(fd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 077) == 0;
}

static bool same_object(const char *name, int fd)
{
	struct stat a, b;
	int cur = shm_open(name, O_RDONLY, 0);
	bool same = cur >= 0 && fstat(fd, &a) == 0 && fstat(cur, &b) == 0 && same_file(&a, &b);
	if (cur >= 0) {
		close(cur);
	}
	return same;
}

/* Unlinks name if no process uses fd's object and name still refers to it. false: in use. */
static bool remove_unused(const char *name, int fd)
{
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		return false;
	}
	if (same_object(name, fd)) {
		shm_unlink(name);
	}
	flock(fd, LOCK_UN);
	return true;
}

/* Marks fd's object as used by this process. false: name was replaced meanwhile, retry. */
static bool hold(const char *name, int fd)
{
	while (flock(fd, LOCK_SH) != 0 && errno == EINTR) {}
	return same_object(name, fd);
}

/* fd mapped exactly at base, or NULL. */
static void *map_at(int fd, size_t size, uintptr_t base)
{
	int flags = MAP_SHARED;
#ifdef MAP_FIXED_NOREPLACE
	flags |= MAP_FIXED_NOREPLACE; /* older kernels treat it as a hint: checked below */
#endif
	void *p = mmap((void *) base, size, PROT_READ | PROT_WRITE, flags, fd, 0);
	if (p == MAP_FAILED) {
		return NULL;
	}
	if ((uintptr_t) p != base) {
		munmap(p, size);
		return NULL;
	}
	return p;
}

/* Far from PIE binaries, heaps, mmap bases and Go's arenas (FrankenPHP); the second fits 39-bit address spaces. */
static const uintptr_t bases[] = {(uintptr_t) 0x200000000000ULL, (uintptr_t) 0x1000000000ULL,
#ifdef __SANITIZE_ADDRESS__
	(uintptr_t) 0x300000000000ULL, /* both above are ASan shadow on aarch64 */
#endif
};

typedef enum { OPEN_OK, OPEN_RETRY, OPEN_FAIL } open_result;

static open_result open_fail(int fd, const char **err, const char *why)
{
	close(fd);
	*err = why;
	return OPEN_FAIL;
}

/* fd: just created (O_EXCL), empty. */
static open_result create_new(const char *name, int fd, const layout *l, uint64_t abi, const char **err)
{
	if (!hold(name, fd)) {
		close(fd);
		return OPEN_RETRY; /* removed as unused before we held it */
	}
#ifdef __linux__
	/* reserve every page now: a sparse tmpfs file over /dev/shm's limit (Docker: 64M) would SIGBUS on first touch */
	int r = posix_fallocate(fd, 0, (off_t) l->size);
	*err = r == ENOSPC ? "not enough room in /dev/shm (Docker: --shm-size)" : r ? "posix_fallocate failed" : NULL;
#else
	*err = ftruncate(fd, (off_t) l->size) ? "ftruncate failed" : NULL;
#endif
	void *p = NULL;
	for (size_t i = 0; !*err && !p && i < sizeof bases / sizeof bases[0]; i++) {
		p = map_at(fd, l->size, bases[i]);
	}
	if (!p) {
		*err = *err ? *err : "no free address for it";
		shm_unlink(name);
		close(fd);
		return OPEN_FAIL;
	}
	nocopy_arena *a = p;
	init(a, l);
	a->abi = abi;
	mapped_size = l->size;
	__atomic_store_n(&a->magic, NOCOPY_MAGIC, __ATOMIC_RELEASE);
	named_fd = fd;
	adopt(a);
	return OPEN_OK;
}

/* fd: an existing object, maybe still being initialized by its creator. */
static open_result attach(const char *name, int fd, uint64_t abi, const char **err)
{
	uint64_t magic = 0, habi = 0;
	uintptr_t base = 0;
	size_t size = 0;
	uint32_t nshards = 0;
	if (!owned(fd)) {
		return open_fail(fd, err, "it is owned by another user or open to others");
	}
	for (int waited = 0;; waited++) {
		struct stat st;
		if (fstat(fd, &st) == 0 && (size_t) st.st_size >= sizeof(nocopy_arena)) {
			nocopy_arena *q = mmap(NULL, sizeof(nocopy_arena), PROT_READ, MAP_SHARED, fd, 0);
			if (q != MAP_FAILED) {
				magic = __atomic_load_n(&q->magic, __ATOMIC_ACQUIRE);
				habi = q->abi;
				base = q->base;
				size = q->size;
				nshards = q->nshards;
				munmap(q, sizeof(nocopy_arena));
			}
		}
		if (magic == NOCOPY_MAGIC) {
			break;
		}
		if (remove_unused(name, fd)) {
			close(fd);
			return OPEN_RETRY; /* its creator died before it was set up */
		}
		if (waited == 2000) {
			return open_fail(fd, err, "its creator has not finished setting it up for 2 s");
		}
		usleep(1000);
	}
	if (habi != abi) {
		return open_fail(fd, err, "it belongs to another build");
	}
	if (!nshards || nshards > NOCOPY_MAX_SHARDS || (nshards & (nshards - 1))) {
		return open_fail(fd, err, "its header is corrupt");
	}
	if (!hold(name, fd)) {
		close(fd);
		return OPEN_RETRY;
	}
	void *p = map_at(fd, size, base);
	if (!p) {
		return open_fail(fd, err, "its address is taken in this process");
	}
	named_fd = fd;
	mapped_size = size;
	adopt(p);
	return OPEN_OK;
}

#ifdef __linux__
#include <dirent.h>

/* Other builds' arenas under the same base (base.<8 hex digits>): unused ones (left by an upgrade) are removed, one still in use is reported. */
static void collect(const char *base, nocopy_named *out)
{
	DIR *d = opendir("/dev/shm");
	if (!d) {
		return;
	}
	size_t bl = strlen(base + 1);
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *n = de->d_name, *x = n + bl + 1;
		if (strncmp(n, base + 1, bl) != 0 /* NOLINT(bugprone-not-null-terminated-result): stops at NUL */ || n[bl] != '.' || strlen(x) != 8 || strspn(x, "0123456789abcdef") != 8 || strcmp(n, out->name + 1) == 0) {
			continue;
		}
		char other[sizeof out->other];
		snprintf(other, sizeof other, "/%.62s", n); /* n is at most 49 chars (checked above) */
		int fd = shm_open(other, O_RDWR, 0);
		if (fd < 0 || !owned(fd)) {
			if (fd >= 0) {
				close(fd);
			}
			continue; /* another user's: not ours to judge */
		}
		if (!remove_unused(other, fd) && !out->other[0]) {
			memcpy(out->other, other, sizeof other);
		}
		close(fd);
	}
	closedir(d);
}
#endif

const char *nocopy_arena_open(const char *base, size_t size, uint64_t hint, uint64_t abi, size_t *need, nocopy_named *out)
{
	memset(out, 0, sizeof *out);
	if (base[0] != '/' || strchr(base + 1, '/') || !base[1] || strlen(base) > 40) {
		return "not a name like /nocopy (one leading slash, at most 40 characters)";
	}
	layout l;
	const char *err = plan(&l, size, hint, need);
	if (err) {
		return err;
	}
	snprintf(out->name, sizeof out->name, "%s.%08x", base, (uint32_t) (abi ^ abi >> 32));
	for (int attempt = 0; attempt < 8; attempt++) {
		int fd = shm_open(out->name, O_RDWR | O_CREAT | O_EXCL, 0600);
		open_result r;
		if (fd >= 0) {
			r = create_new(out->name, fd, &l, abi, &err);
		} else if (errno != EEXIST) {
			return errno == EACCES ? "no permission to create it" : "shm_open failed";
		} else if ((fd = shm_open(out->name, O_RDWR, 0)) < 0) {
			if (errno == ENOENT) {
				continue; /* removed meanwhile */
			}
			return errno == EACCES ? "no permission to open it (another user created it)" : "shm_open failed";
		} else {
			r = attach(out->name, fd, abi, &err);
		}
		if (r == OPEN_FAIL) {
			return err;
		}
		if (r == OPEN_OK) {
			memcpy(named_name, out->name, sizeof named_name);
#ifdef __linux__
			collect(base, out);
#endif
			return NULL;
		}
	}
	return "it was replaced again and again while opening it";
}

void nocopy_arena_unhold(void)
{
	if (named_fd >= 0 && fstat(named_fd, &named_st) == 0) {
		close(named_fd); /* the flock stays with the other processes sharing the open file */
		named_fd = -1;
	}
}

bool nocopy_arena_current(void)
{
	struct stat b;
#ifdef __linux__
	char path[80];
	snprintf(path, sizeof path, "/dev/shm%s", named_name); /* stat needs no permission on the object: fpm workers run as another user */
	return stat(path, &b) == 0 && same_file(&b, &named_st);
#else
	int cur = shm_open(named_name, O_RDONLY, 0);
	bool same = cur >= 0 && fstat(cur, &b) == 0 && same_file(&b, &named_st);
	if (cur >= 0) {
		close(cur);
	} else if (errno == EACCES) {
		return true; /* it exists, not ours to open: assume it is still ours */
	}
	return same;
#endif
}

void nocopy_arena_destroy(void)
{
	if (nocopy_a) {
		munmap(nocopy_a, mapped_size);
		nocopy_a = NULL;
	}
	if (named_fd >= 0) {
		close(named_fd); /* drops our shared flock */
		named_fd = -1;
	}
}
