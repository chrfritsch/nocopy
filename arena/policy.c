/* nocopy arena: the eviction policy: ghost, merge victims, survivor ranking. */

#include "policy.h"

#define MERGE_N 4 /* segments merged into at most one segment's worth of survivors */
#define PICK_N 32 /* eviction chooses among this many oldest segments */
#define COMPACT_MAX (ar_seg_size / 4 * 3) /* a segment with more live bytes is no compaction victim */

static uint64_t *expunges;
static uint32_t ghost_mask;

void ar_policy_adopt(uint64_t *x, uint32_t ghost_slots)
{
	expunges = x;
	ghost_mask = ghost_slots - 1;
}

void ar_policy_count_expunge(void) { __atomic_add_fetch(expunges, 1, __ATOMIC_RELAXED); }

/* ---- ghost ---- */

/* Direct-mapped: a newer eviction overwrites an older one in its cell. Index: Fibonacci mix below the shard's top 8 bits
 * (short keys' DJB hashes have few high bits); tag: the low bits (never 0). */
static uint16_t *ghost_cell(uint16_t *ghosts, uint64_t h) { return &ghosts[(uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 24) & ghost_mask]; }
static uint16_t ghost_tag(uint64_t h) { return (uint16_t) h | 1; }

uint8_t ar_policy_admit(ar_policy *pol, uint16_t *ghosts, uint64_t h)
{
	uint16_t *c = ghost_cell(ghosts, h);
	if (*c != ghost_tag(h)) {
		return 0;
	}
	*c = 0;
	pol->ghost_hits++;
	return 2;
}

/* ---- merge ---- */

/* Would survive a merge: published and not expired. */
static bool alive(const nocopy_entry *e) { return e->live && !nocopy_expired(e); }

static uint64_t seg_live(uint32_t s) { return ar_segs[s].live; }
static uint64_t seg_born(uint32_t s) { return ar_segs[s].born; }

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
	ar_seg *g = &ar_segs[s];
	return g->state == SEG_SEALED && g->run == 1 && !g->pending;
}

/* Holds one entry over half a segment (open segments take none): moving it takes a whole segment again, so compacting frees nothing. */
static bool seg_big(uint32_t s) { return nocopy_entry_size((const nocopy_entry *) ar_seg_start(s)) > ar_seg_size / 2; }

/* Alone, s frees only its dead bytes (the copy has a tail too): at most 3 bytes copied per byte freed, as a full segment at COMPACT_MAX. */
static bool worth_alone(uint32_t s) { return 3 * (ar_segs[s].used - ar_segs[s].live) >= ar_segs[s].live; }

/* Compaction victims: the sparsest sealed segments, together <= one segment (*live), or the sparsest alone if that frees enough. */
static uint32_t pick_sparse(const ar_log *lg, uint32_t *v, size_t *live)
{
	uint32_t nv = 0;
	AR_LOG_SEGS(lg, s) {
		if (mergeable(s) && seg_live(s) <= COMPACT_MAX && !seg_big(s)) {
			keep_least(v, &nv, MERGE_N, s, seg_live);
		}
	}
	*live = 0;
	uint32_t n = 0;
	for (; n < nv && *live + ar_segs[v[n]].live <= ar_seg_size; n++) {
		*live += ar_segs[v[n]].live;
	}
	return n >= 2 || (n && worth_alone(v[0])) ? n : 0;
}

/* The K oldest sealed segments, oldest first, or the oldest big entry's run alone if it is older.
 * A run that was read since is sent to the back once instead (*spared, no victims). */
static uint32_t pick_oldest(const ar_merge *m, uint32_t *v, bool *spared, uint32_t K)
{
	uint32_t nv = 0, run = AR_NONE;
	*spared = false;
	AR_LOG_SEGS(m->log, s) {
		ar_seg *g = &ar_segs[s];
		if (g->state != SEG_SEALED || g->pending) {
			continue;
		}
		if (g->run > 1) {
			if (run == AR_NONE || g->born < ar_segs[run].born) {
				run = s;
			}
			continue;
		}
		keep_least(v, &nv, K, s, seg_born);
	}
	if (run == AR_NONE || (nv && ar_segs[run].born >= ar_segs[v[0]].born)) {
		return nv;
	}
	nocopy_entry *e = (nocopy_entry *) ar_seg_start(run);
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (e->live && f) {
		__atomic_store_n(&e->hot, f >> 1, __ATOMIC_RELAXED);
		ar_log_requeue(run, m->last_seq);
		*spared = true;
		return 0;
	}
	v[0] = run;
	return 1;
}

/* Eviction victims: of the PICK_N oldest, the oldest (so every segment is merged in time) and the MERGE_N - 1 least read. */
static uint32_t pick_victims(const ar_merge *m, uint32_t *v, bool *spared)
{
	uint32_t c[PICK_N];
	uint32_t nc = pick_oldest(m, c, spared, PICK_N);
	for (uint32_t n = 1; n < MERGE_N && n < nc; n++) {
		uint32_t b = n;
		for (uint32_t i = n + 1; i < nc; i++) {
			if (__atomic_load_n(&ar_segs[c[i]].reads, __ATOMIC_RELAXED) < __atomic_load_n(&ar_segs[c[b]].reads, __ATOMIC_RELAXED)) {
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
		AR_SEG_FOREACH(v[i], e) {
			if (alive(e)) {
				by[__atomic_load_n(&e->hot, __ATOMIC_RELAXED)] += nocopy_entry_size(e);
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

/* Moves survivors forward until `budget` bytes are used: every live entry when compacting, the most-read ones when evicting.
 * A moved copy keeps half its read count (aging), so it must be read again to stay on top. */
static void move_survivors(const ar_merge *m, const uint32_t *v, uint32_t nv, size_t budget, bool lossy)
{
	size_t spare = 0;
	uint8_t cut = lossy && budget ? rank_cut(v, nv, budget, &spare) : 1;
	for (uint32_t i = 0; i < nv && budget; i++) {
		AR_SEG_FOREACH(v[i], e) {
			if (!budget) {
				break;
			}
			if (!alive(e)) {
				continue;
			}
			uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED); /* readers may raise it meanwhile: cut is a hint */
			if (lossy) {
				if (f < cut - 1 || f == 0) {
					continue;
				}
				if (f < cut) {
					if (nocopy_entry_size(e) > spare) {
						continue;
					}
					spare -= nocopy_entry_size(e);
				}
			}
			size_t size = nocopy_entry_size(e);
			if (size > budget || !m->relocate(e, f >> 1, m->arg)) {
				budget = 0;
				break;
			}
			m->pol->evacuated += size;
			budget -= size;
		}
	}
}

/* Unlinks what the victims still hold: expired entries count as expired, the rest as evicted (leaving a ghost). */
static void evict_rest(const ar_merge *m, const uint32_t *v, uint32_t nv)
{
	bool lost = false;
	for (uint32_t i = 0; i < nv && !lost; i++) {
		AR_SEG_FOREACH(v[i], e) {
			if (alive(e)) {
				lost = true;
				break;
			}
		}
	}
	if (lost) {
		ar_policy_count_expunge();
	}
	for (uint32_t i = 0; i < nv; i++) {
		AR_SEG_FOREACH(v[i], e) {
			if (!e->live) {
				continue;
			}
			if (nocopy_expired(e)) {
				ar_log_count_expired(m->log);
			} else {
				m->pol->evicted++;
				*ghost_cell(m->ghosts, e->h) = ghost_tag(e->h);
			}
			m->unlink(e, m->arg);
			AR_HOOK("merge.evicted");
		}
	}
}

bool ar_policy_merge(const ar_merge *m, bool evict_only)
{
	uint32_t v[MERGE_N], nv = 0;
	size_t budget = 0;
	if (!evict_only) {
		nv = pick_sparse(m->log, v, &budget);
	}
	bool lossy = !nv;
	if (lossy) {
		bool spared;
		nv = pick_victims(m, v, &spared);
		if (spared) {
			return true;
		}
		if (!nv) {
			return false;
		}
		budget = nv > 2 ? ar_seg_size : 0; /* survivors fill at most one segment: frees at least one */
	}
	AR_HOOK("merge.picked");
	move_survivors(m, v, nv, budget, lossy);
	AR_HOOK("merge.moved");
	evict_rest(m, v, nv);
	return true;
}
