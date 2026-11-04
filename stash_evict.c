/* stash arena: incremental expiry and merge (Segcache: compaction and eviction in one pass). Shard locked throughout. */

#include "stash_arena_int.h"

#define MERGE_N 4 /* segments merged into at most one segment's worth of survivors */

/* The entries of segment s. A walk may run past unlinks that make s DEAD: only reclaim() frees it, under this lock. */
#define SEG_FOREACH(s, e) \
	for (stash_entry *e = (stash_entry *) seg_start(s), *e##_end = (stash_entry *) (seg_start(s) + stash_a->segs[s].used); \
	     e < e##_end; e = (stash_entry *) ((char *) e + e->size))

/* ---- expiry ---- */

/* Unlinks entries of sh's segments whose entries have all expired, looking at most `budget` entries per call. */
bool stash_expire_step(stash_shard *sh, time_t now, int budget)
{
	bool did = false;
	if (sh->exp_idle == now) {
		return false;
	}
	uint32_t n = sh->hi - sh->lo;
	if (sh->exp_next < sh->lo || sh->exp_next >= sh->hi) {
		sh->exp_next = sh->lo;
	}
	for (uint32_t seen = 0; seen < n; seen++) {
		uint32_t s = sh->exp_next;
		stash_seg *g = &stash_a->segs[s];
		if ((g->state == SEG_OPEN || g->state == SEG_SEALED) && g->live && g->max_exp && g->max_exp < now) {
			if (sh->exp_gen != g->gen) { /* freed and reused since the cursor stopped here: entry boundaries moved */
				sh->exp_gen = g->gen;
				sh->exp_off = 0;
			}
			while (sh->exp_off < g->used) {
				if (budget-- <= 0) {
					return did; /* resume here on the next write */
				}
				stash_entry *e = (stash_entry *) (seg_start(s) + sh->exp_off);
				sh->exp_off += e->size;
				if (e->live) {
					stash_entry_unlink(sh, e);
					sh->expired++;
					did = true;
				}
			}
		}
		sh->exp_off = 0;
		sh->exp_next = s + 1 < sh->hi ? s + 1 : sh->lo;
	}
	sh->exp_idle = now;
	return did;
}

/* ---- merge ---- */

/* Copies e forward into its segment's TTL bucket and repoints its slot. False if there is no room. */
static bool relocate(stash_shard *sh, stash_entry *old)
{
	stash_entry *n = stash_seg_alloc(sh, old->size, true, stash_a->segs[stash_seg_of(old)].bucket, old->expires);
	if (!n) {
		return false;
	}
	memcpy(n + 1, old + 1, old->size - sizeof *old);
	n->h = old->h;
	n->expires = old->expires;
	n->created = old->created;
	n->seq = old->seq;
	n->klen = old->klen;
	n->koff = old->koff;
	n->flags = old->flags;
	n->user = old->user;
	n->hot = __atomic_load_n(&old->hot, __ATOMIC_RELAXED) >> 1; /* aging: must be read again to stay on top */
	stash_rebase(n, old);
	sh->evacuated += n->size;
	stash_slot *sp = stash_slot_of(sh, old);
	if (sp) {
		stash_entry_birth(sh, n);
		__atomic_store_n(sp, slot_make(n), __ATOMIC_RELEASE);
	} else {
		stash_abort(sh, n);
	}
	stash_entry_die(sh, old);
	return true;
}

static bool mergeable(uint32_t s)
{
	stash_seg *g = &stash_a->segs[s];
	return g->state == SEG_SEALED && g->run == 1 && !g->pending;
}

/* Compaction victims: sealed segments at <= 1/4 live, together <= one segment (*live). 0 if fewer than two: one is just moved, not freed. */
static uint32_t pick_sparse(const stash_shard *sh, uint32_t *v, size_t *live)
{
	uint32_t nv = 0;
	*live = 0;
	for (uint32_t s = sh->lo; s < sh->hi && nv < MERGE_N; s++) {
		size_t l = stash_a->segs[s].live;
		if (mergeable(s) && l <= stash_seg_size / 4 && *live + l <= stash_seg_size) {
			*live += l;
			v[nv++] = s;
		}
	}
	return nv >= 2 ? nv : 0;
}

/* Eviction victims, oldest first: the MERGE_N oldest sealed segments, or the oldest big entry's run alone if it is older.
 * A run that was read since is sent to the back once instead (*spared, no victims). */
static uint32_t pick_oldest(const stash_shard *sh, uint32_t *v, bool *spared)
{
	uint32_t nv = 0, run = UINT32_MAX;
	*spared = false;
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		stash_seg *g = &stash_a->segs[s];
		if (g->state != SEG_SEALED || g->pending) {
			continue;
		}
		if (g->run > 1) {
			if (run == UINT32_MAX || g->born < stash_a->segs[run].born) {
				run = s;
			}
			continue;
		}
		if (nv == MERGE_N && g->born >= stash_a->segs[v[MERGE_N - 1]].born) {
			continue;
		}
		uint32_t k = nv < MERGE_N ? nv++ : MERGE_N - 1; /* keep v sorted by born */
		for (; k > 0 && stash_a->segs[v[k - 1]].born > g->born; k--) {
			v[k] = v[k - 1];
		}
		v[k] = s;
	}
	if (run == UINT32_MAX || (nv && stash_a->segs[run].born >= stash_a->segs[v[0]].born)) {
		return nv;
	}
	stash_entry *e = (stash_entry *) seg_start(run);
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (e->live && f) {
		__atomic_store_n(&e->hot, f >> 1, __ATOMIC_RELAXED);
		stash_a->segs[run].born = __atomic_load_n(&stash_a->inserts, __ATOMIC_RELAXED);
		*spared = true;
		return 0;
	}
	v[0] = run;
	return 1;
}

/* Lossy merge ranking: levels >= the returned cut all fit the budget; level cut - 1 gets *spare (first come). */
static uint8_t rank_cut(const uint32_t *v, uint32_t nv, size_t budget, size_t *spare)
{
	size_t by[STASH_HOT_MAX + 1] = {0};
	for (uint32_t i = 0; i < nv; i++) {
		SEG_FOREACH(v[i], e) {
			if (e->live && !stash_expired(e)) {
				by[__atomic_load_n(&e->hot, __ATOMIC_RELAXED)] += e->size;
			}
		}
	}
	size_t sum = 0;
	uint8_t cut = STASH_HOT_MAX + 1;
	for (; cut > 1 && sum + by[cut - 1] <= budget; cut--) {
		sum += by[cut - 1];
	}
	*spare = budget - sum;
	return cut;
}

/* Moves survivors forward until `budget` bytes are used: every live entry when compacting, the most-read ones when evicting. */
static void move_survivors(stash_shard *sh, const uint32_t *v, uint32_t nv, size_t budget, bool lossy)
{
	size_t spare = 0;
	uint8_t cut = lossy && budget ? rank_cut(v, nv, budget, &spare) : 1;
	for (uint32_t i = 0; i < nv && budget; i++) {
		SEG_FOREACH(v[i], e) {
			if (!budget) {
				break;
			}
			if (!e->live || stash_expired(e)) {
				continue;
			}
			if (lossy) {
				uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED); /* readers may raise it meanwhile: cut is a hint */
				if (f < cut - 1 || f == 0) {
					continue;
				}
				if (f < cut) {
					if (e->size > spare) {
						continue;
					}
					spare -= e->size;
				}
			}
			if (e->size > budget || !relocate(sh, e)) {
				budget = 0;
				break;
			}
			budget -= e->size;
		}
	}
}

/* Unlinks what the victims still hold: expired entries count as expired, the rest as evicted (leaving a ghost). */
static void evict_rest(stash_shard *sh, const uint32_t *v, uint32_t nv)
{
	bool lost = false;
	for (uint32_t i = 0; i < nv && !lost; i++) {
		SEG_FOREACH(v[i], e) {
			if (e->live && !stash_expired(e)) {
				lost = true;
				break;
			}
		}
	}
	if (lost) {
		__atomic_add_fetch(&stash_a->expunges, 1, __ATOMIC_RELAXED); /* before the unlinks: a reader that misses then sees it */
	}
	for (uint32_t i = 0; i < nv; i++) {
		SEG_FOREACH(v[i], e) {
			if (!e->live) {
				continue;
			}
			if (stash_expired(e)) {
				sh->expired++;
			} else {
				sh->evicted++;
				*ghost_cell(sh, e->h) = ghost_tag(e->h);
			}
			stash_entry_unlink(sh, e);
		}
	}
}

/* Frees memory (or slots) in sh: compacts sparse segments losslessly, else evicts the oldest, keeping their most-read entries
 * (a moved copy keeps half its read count, so it must be read again to stay on top). evict_only: the slot table is full,
 * compaction would not free any. Victims that readers pin die but stay allocated until unpinned; the next call moves on. */
bool stash_merge(stash_shard *sh, bool evict_only)
{
	uint32_t v[MERGE_N], nv = 0;
	size_t budget = 0;
	if (!evict_only) {
		nv = pick_sparse(sh, v, &budget);
	}
	bool lossy = !nv;
	if (lossy) {
		bool spared;
		nv = pick_oldest(sh, v, &spared);
		if (spared) {
			return true;
		}
		if (!nv) {
			return false;
		}
		budget = nv > 2 ? stash_seg_size : 0; /* survivors fill at most one segment: frees at least one */
	}
	move_survivors(sh, v, nv, budget, lossy);
	evict_rest(sh, v, nv);
	return true;
}
