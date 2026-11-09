/* nocopy arena: a shard's log: segments in and out of the pool, appends, expiry, repair. */

#include "log.h"

ar_seg *ar_segs;
static uint32_t keep_free; /* segments a plain take leaves in the pool: one per shard, so every merge can move survivors */

void ar_log_init(ar_log *lg, uint8_t tenant, uint32_t home)
{
	lg->tenant = tenant;
	lg->home = home;
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		lg->cur[b] = -1;
	}
}

void ar_log_adopt(ar_seg *segs, uint32_t nlogs)
{
	ar_segs = segs;
	keep_free = nlogs;
}

int ar_log_bucket(time_t ttl)
{
	int b = ttl ? 1 : 0;
	while (b && b < NOCOPY_TTL_BUCKETS - 1 && ttl >= ((time_t) 1 << (3 * b))) {
		b++;
	}
	return b;
}

/* k contiguous segments from the pool, the first in `state`; AR_NONE if none. */
static uint32_t take(ar_log *lg, uint32_t k, bool reserve, uint8_t state, uint64_t born)
{
	uint32_t got = ar_pool_take(lg->tenant, k, lg->home, reserve ? 0 : keep_free);
	if (got == AR_NONE) {
		return AR_NONE;
	}
	for (uint32_t j = got; j < got + k; j++) {
		ar_segs_add(lg->own, j);
	}
	__atomic_store_n(&lg->nown, lg->nown + k, __ATOMIC_RELAXED);
	for (uint32_t j = got + 1; j < got + k; j++) {
		ar_seg_set(j, SEG_CONT);
	}
	ar_segs[got].run = k;
	ar_segs[got].born = born;
	ar_seg_set(got, state);
	return got;
}

/* Out of line: its registers cost the fast path a longer prologue (+5 instructions a write). */
__attribute__((noinline)) nocopy_entry *ar_log_alloc_new(ar_log *lg, size_t total, int bucket, time_t exp, uint64_t born, bool reserve)
{
	uint32_t got;
	if (total > ar_seg_size / 2) { /* a run of contiguous segments */
		got = take(lg, (uint32_t) ((total + ar_seg_size - 1) >> ar_seg_shift), reserve, SEG_SEALED, born);
		return got == AR_NONE ? NULL : ar_log_carve(got, total, exp);
	}
	got = take(lg, 1, reserve, SEG_OPEN, born);
	if (got == AR_NONE) {
		return NULL;
	}
	ar_log_seal(lg, bucket);
	ar_segs[got].bucket = (uint8_t) bucket;
	lg->cur[bucket] = (int32_t) got;
	return ar_log_carve(got, total, exp);
}

void ar_log_seal(ar_log *lg, int bucket)
{
	int32_t cur = lg->cur[bucket];
	if (cur >= 0) {
		ar_seg_set((uint32_t) cur, SEG_SEALED);
		ar_log_check(lg, (uint32_t) cur);
		lg->cur[bucket] = -1;
	}
}

void ar_log_seal_all(ar_log *lg)
{
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		ar_log_seal(lg, b);
	}
}

/* Segments s..s+k-1 leave lg: records zeroed, then back to the pool. */
static void give(ar_log *lg, uint32_t s, uint32_t k)
{
	for (uint32_t j = s; j < s + k; j++) {
		ar_segs[j] = (ar_seg) {0}; /* SEG_FREE */
		ar_segs_del(lg->own, j);
		__atomic_store_n(&lg->nown, lg->nown - 1, __ATOMIC_RELAXED);
	}
	ar_pool_give(s, k);
}

/* DEAD ends only here, under this lock. */
void ar_log_reclaim(ar_log *lg)
{
	lg->hint = false;
	if (!lg->ndead) {
		return;
	}
	uint64_t dead[NOCOPY_SEG_WORDS] = {0}, held[NOCOPY_SEG_WORDS];
	AR_LOG_SEGS(lg, s) {
		if (ar_segs[s].state == SEG_DEAD) {
			ar_segs_add(dead, s);
		}
	}
	ar_pool_pinned(dead, held, nocopy_now());
	bool blocked = false;
	AR_LOG_SEGS(lg, s) {
		if (!ar_segs_has(dead, s)) {
			continue;
		}
		if (ar_segs_has(held, s)) {
			blocked = true;
		} else {
			give(lg, s, ar_segs[s].run);
			lg->ndead--;
		}
	}
	if (blocked) {
		ar_pool_ask_unpin();
	}
}

bool ar_log_expire_step(ar_log *lg, time_t now, int budget, void (*unlink)(nocopy_entry *e, void *arg), void *arg)
{
	bool did = false;
	if (lg->exp_idle == now) {
		return false;
	}
	uint32_t n = ar_nsegs;
	if (lg->exp_next >= n) {
		lg->exp_next = 0;
	}
	for (uint32_t seen = 0; seen < n; seen++) {
		uint32_t s = lg->exp_next;
		ar_seg *g = &ar_segs[s];
		if (ar_log_owns(lg, s) && (g->state == SEG_OPEN || g->state == SEG_SEALED) && g->live && g->max_exp && g->max_exp < now) {
			if (lg->exp_gen != ar_seg_gen(s)) { /* freed and reused since the cursor stopped here: entry boundaries moved */
				lg->exp_gen = ar_seg_gen(s);
				lg->exp_off = 0;
			}
			while (lg->exp_off < g->used) {
				if (budget-- <= 0) {
					return did; /* resume here on the next write */
				}
				nocopy_entry *e = (nocopy_entry *) (ar_seg_start(s) + lg->exp_off);
				lg->exp_off += e->size;
				if (e->live) {
					unlink(e, arg);
					lg->expired++;
					did = true;
				}
			}
		}
		lg->exp_off = 0;
		lg->exp_next = s + 1 < n ? s + 1 : 0;
	}
	lg->exp_idle = now;
	return did;
}

void ar_log_rebuild(ar_log *lg, bool (*linked)(const nocopy_entry *e, void *arg), void *arg)
{
	memset(lg->own, 0, sizeof lg->own); /* owner[] is the truth: a claim or release may have stopped halfway */
	uint32_t nown = 0;
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		if (ar_pool_tenant(s) == lg->tenant) {
			ar_segs_add(lg->own, s);
			nown++;
		}
	}
	__atomic_store_n(&lg->nown, nown, __ATOMIC_RELAXED);
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		int32_t c = lg->cur[b];
		if (c >= 0 && (!ar_log_owns(lg, (uint32_t) c) || ar_segs[c].state != SEG_OPEN)) {
			lg->cur[b] = -1;
		}
	}
	lg->ndead = 0;
	lg->live_bytes = 0;
	for (uint32_t s = ar_log_next(lg, 0); s != AR_NONE;) {
		ar_seg *g = &ar_segs[s];
		if (g->state == SEG_FREE || g->state == SEG_CONT) { /* claimed but not set up, or CONT outside any run: a run taken or freed halfway */
			give(lg, s, 1);
			s = ar_log_next(lg, s + 1);
			continue;
		}
		uint32_t k = 1;
		while (k < g->run && s + k < ar_nsegs && ar_log_owns(lg, s + k)) {
			k++;
		}
		g->run = k; /* give() releases run segments: never more than it holds */
		for (uint32_t j = s + 1; j < s + k; j++) {
			ar_seg_set(j, SEG_CONT);
		}
		bool installed = false;
		for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
			installed |= lg->cur[b] == (int32_t) s;
		}
		if (g->state == SEG_OPEN && !installed) {
			ar_seg_set(s, SEG_SEALED);
		}
		g->pending = 0;
		if (g->state != SEG_DEAD) {
			g->live = 0;
			for (char *p = ar_seg_start(s), *end = p + g->used; p < end;) {
				nocopy_entry *e = (nocopy_entry *) p;
				if (e->size < sizeof *e) {
					break;
				}
				e->live = linked(e, arg);
				g->live += e->live ? e->size : 0;
				p += e->size;
			}
			lg->live_bytes += g->live;
		}
		s = ar_log_next(lg, s + k);
	}
}

void ar_log_settle(ar_log *lg)
{
	AR_LOG_SEGS(lg, s) {
		if (ar_segs[s].state == SEG_DEAD) {
			lg->ndead++;
		} else {
			ar_log_check(lg, s);
		}
	}
	lg->exp_off = 0;
	lg->exp_idle = 0;
	ar_log_reclaim(lg);
}
