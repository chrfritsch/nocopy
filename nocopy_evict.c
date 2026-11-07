/* nocopy arena: incremental expiry and merge (Segcache: compaction and eviction in one pass). Shard locked throughout. */

#include "nocopy_arena_int.h"

#define MERGE_N 4 /* segments merged into at most one segment's worth of survivors */
#define PICK_N 32 /* eviction chooses among this many oldest segments */
#define COMPACT_MAX (nocopy_seg_size / 4 * 3) /* a segment this full or less is compacted alone rather than evicting */

/* The entries of segment s. A walk may run past unlinks that make s DEAD: only reclaim() frees it, under this lock. */
#define SEG_FOREACH(s, e) \
	for (nocopy_entry *e = (nocopy_entry *) seg_start(s), *e##_end = (nocopy_entry *) (seg_start(s) + nocopy_segs[s].used); \
	     e < e##_end; e = (nocopy_entry *) ((char *) e + e->size))

/* ---- expiry ---- */

/* Unlinks entries of sh's segments whose entries have all expired, looking at most `budget` entries per call. */
bool nocopy_expire_step(nocopy_shard *sh, time_t now, int budget)
{
	bool did = false;
	if (sh->exp_idle == now) {
		return false;
	}
	uint32_t n = nocopy_a->nsegs;
	if (sh->exp_next >= n) {
		sh->exp_next = 0;
	}
	for (uint32_t seen = 0; seen < n; seen++) {
		uint32_t s = sh->exp_next;
		nocopy_seg *g = &nocopy_segs[s];
		if (owns(sh, s) && (g->state == SEG_OPEN || g->state == SEG_SEALED) && g->live && g->max_exp && g->max_exp < now) {
			if (sh->exp_gen != g->gen) { /* freed and reused since the cursor stopped here: entry boundaries moved */
				sh->exp_gen = g->gen;
				sh->exp_off = 0;
			}
			while (sh->exp_off < g->used) {
				if (budget-- <= 0) {
					return did; /* resume here on the next write */
				}
				nocopy_entry *e = (nocopy_entry *) (seg_start(s) + sh->exp_off);
				sh->exp_off += e->size;
				if (e->live) {
					nocopy_entry_unlink(sh, e);
					sh->expired++;
					did = true;
				}
			}
		}
		sh->exp_off = 0;
		sh->exp_next = s + 1 < n ? s + 1 : 0;
	}
	sh->exp_idle = now;
	return did;
}

/* ---- merge ---- */

/* Copies e forward into its segment's TTL bucket and repoints its slot. False if there is no room. */
static bool relocate(nocopy_shard *sh, nocopy_entry *old)
{
	nocopy_entry *n = nocopy_seg_alloc(sh, old->size, true, nocopy_segs[nocopy_seg_of(old)].bucket, old->expires);
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
	nocopy_rebase(n, old);
	sh->evacuated += n->size;
	nocopy_slot *sp = nocopy_slot_of(sh, old);
	if (sp) {
		nocopy_entry_birth(sh, n);
		__atomic_store_n(sp, slot_make(n), __ATOMIC_RELEASE);
	} else {
		nocopy_abort(sh, n);
	}
	nocopy_entry_die(sh, old);
	return true;
}

/* Would survive a merge: published and not expired. */
static bool alive(const nocopy_entry *e) { return e->live && !nocopy_expired(e); }

static uint64_t seg_live(uint32_t s) { return nocopy_segs[s].live; }
static uint64_t seg_born(uint32_t s) { return nocopy_segs[s].born; }

/* Keeps v (*nv of at most k) the k segments with the smallest key, ascending: s goes in if it beats the largest. */
static void keep_least(uint32_t *v, uint32_t *nv, uint32_t k, uint32_t s, uint64_t (*key)(uint32_t))
{
	uint64_t ks = key(s);
	if (*nv == k && ks >= key(v[k - 1])) {
		return;
	}
	uint32_t i = *nv < k ? (*nv)++ : k - 1;
	for (; i > 0 && key(v[i - 1]) > ks; i--) {
		v[i] = v[i - 1];
	}
	v[i] = s;
}

static bool mergeable(uint32_t s)
{
	nocopy_seg *g = &nocopy_segs[s];
	return g->state == SEG_SEALED && g->run == 1 && !g->pending;
}

/* Holds one entry over half a segment (open segments take none): moving it takes a whole segment again, so compacting frees nothing. */
static bool seg_big(uint32_t s) { return ((const nocopy_entry *) seg_start(s))->size > nocopy_seg_size / 2; }

/* Compaction victims: the sparsest sealed segments, together <= one segment (*live), or the sparsest alone up to COMPACT_MAX. */
static uint32_t pick_sparse(const nocopy_shard *sh, uint32_t *v, size_t *live)
{
	uint32_t nv = 0;
	SH_SEGS(sh, s) {
		if (mergeable(s) && seg_live(s) <= COMPACT_MAX && !seg_big(s)) {
			keep_least(v, &nv, MERGE_N, s, seg_live);
		}
	}
	*live = 0;
	uint32_t n = 0;
	for (; n < nv && *live + nocopy_segs[v[n]].live <= nocopy_seg_size; n++) {
		*live += nocopy_segs[v[n]].live;
	}
	return n >= 2 || (n && *live <= COMPACT_MAX) ? n : 0;
}

/* The K oldest sealed segments, oldest first, or the oldest big entry's run alone if it is older.
 * A run that was read since is sent to the back once instead (*spared, no victims). */
static uint32_t pick_oldest(const nocopy_shard *sh, uint32_t *v, bool *spared, uint32_t K)
{
	uint32_t nv = 0, run = UINT32_MAX;
	*spared = false;
	SH_SEGS(sh, s) {
		nocopy_seg *g = &nocopy_segs[s];
		if (g->state != SEG_SEALED || g->pending) {
			continue;
		}
		if (g->run > 1) {
			if (run == UINT32_MAX || g->born < nocopy_segs[run].born) {
				run = s;
			}
			continue;
		}
		keep_least(v, &nv, K, s, seg_born);
	}
	if (run == UINT32_MAX || (nv && nocopy_segs[run].born >= nocopy_segs[v[0]].born)) {
		return nv;
	}
	nocopy_entry *e = (nocopy_entry *) seg_start(run);
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (e->live && f) {
		__atomic_store_n(&e->hot, f >> 1, __ATOMIC_RELAXED);
		nocopy_segs[run].born = sh->last_seq;
		*spared = true;
		return 0;
	}
	v[0] = run;
	return 1;
}

/* Eviction victims: of the PICK_N oldest, the oldest (so every segment is merged in time) and the MERGE_N - 1 least read. */
static uint32_t pick_victims(const nocopy_shard *sh, uint32_t *v, bool *spared)
{
	uint32_t c[PICK_N];
	uint32_t nc = pick_oldest(sh, c, spared, PICK_N);
	for (uint32_t n = 1; n < MERGE_N && n < nc; n++) {
		uint32_t b = n;
		for (uint32_t i = n + 1; i < nc; i++) {
			if (__atomic_load_n(&nocopy_segs[c[i]].reads, __ATOMIC_RELAXED) < __atomic_load_n(&nocopy_segs[c[b]].reads, __ATOMIC_RELAXED)) {
				b = i;
			}
		}
		uint32_t t = c[n];
		c[n] = c[b];
		c[b] = t;
	}
	nc = nc < MERGE_N ? nc : MERGE_N;
	memcpy(v, c, nc * sizeof *c);
	return nc;
}

/* Lossy merge ranking: levels >= the returned cut all fit the budget; level cut - 1 gets *spare (first come). */
static uint8_t rank_cut(const uint32_t *v, uint32_t nv, size_t budget, size_t *spare)
{
	size_t by[NOCOPY_HOT_MAX + 1] = {0};
	for (uint32_t i = 0; i < nv; i++) {
		SEG_FOREACH(v[i], e) {
			if (alive(e)) {
				by[__atomic_load_n(&e->hot, __ATOMIC_RELAXED)] += e->size;
			}
		}
	}
	size_t sum = 0;
	uint8_t cut = NOCOPY_HOT_MAX + 1;
	for (; cut > 1 && sum + by[cut - 1] <= budget; cut--) {
		sum += by[cut - 1];
	}
	*spare = budget - sum;
	return cut;
}

/* Moves survivors forward until `budget` bytes are used: every live entry when compacting, the most-read ones when evicting. */
static void move_survivors(nocopy_shard *sh, const uint32_t *v, uint32_t nv, size_t budget, bool lossy)
{
	size_t spare = 0;
	uint8_t cut = lossy && budget ? rank_cut(v, nv, budget, &spare) : 1;
	for (uint32_t i = 0; i < nv && budget; i++) {
		SEG_FOREACH(v[i], e) {
			if (!budget) {
				break;
			}
			if (!alive(e)) {
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
static void evict_rest(nocopy_shard *sh, const uint32_t *v, uint32_t nv)
{
	bool lost = false;
	for (uint32_t i = 0; i < nv && !lost; i++) {
		SEG_FOREACH(v[i], e) {
			if (alive(e)) {
				lost = true;
				break;
			}
		}
	}
	if (lost) {
		__atomic_add_fetch(&nocopy_a->expunges, 1, __ATOMIC_RELAXED); /* before the unlinks: a reader that misses then sees it */
	}
	for (uint32_t i = 0; i < nv; i++) {
		SEG_FOREACH(v[i], e) {
			if (!e->live) {
				continue;
			}
			if (nocopy_expired(e)) {
				sh->expired++;
			} else {
				sh->evicted++;
				*ghost_cell(sh, e->h) = ghost_tag(e->h);
			}
			nocopy_entry_unlink(sh, e);
		}
	}
}

/* Frees memory (or slots) in sh: compacts sparse segments losslessly, else evicts the oldest, keeping their most-read entries
 * (a moved copy keeps half its read count, so it must be read again to stay on top). evict_only: the slot table is full,
 * compaction would not free any. Victims that readers pin die but stay allocated until unpinned; the next call moves on. */
bool nocopy_merge(nocopy_shard *sh, bool evict_only)
{
	uint32_t v[MERGE_N], nv = 0;
	size_t budget = 0;
	if (!evict_only) {
		nv = pick_sparse(sh, v, &budget);
	}
	bool lossy = !nv;
	if (lossy) {
		bool spared;
		nv = pick_victims(sh, v, &spared);
		if (spared) {
			return true;
		}
		if (!nv) {
			return false;
		}
		budget = nv > 2 ? nocopy_seg_size : 0; /* survivors fill at most one segment: frees at least one */
	}
	move_survivors(sh, v, nv, budget, lossy);
	evict_rest(sh, v, nv);
	return true;
}
