/* nocopy arena: making room in a shard, cheapest first: expiry, dead segments, segments other shards hold beyond their
 * share, merges (Segcache: compaction and eviction in one pass; read counts per entry and segment, and the ghost of
 * recently evicted keys, S3-FIFO). The only place that chooses victims. Shard locked throughout. */

#include "shard_int.h"

#define MERGE_N 4 /* segments merged into at most one segment's worth of survivors */
#define PICK_N 32 /* eviction chooses among this many oldest segments */
#define COMPACT_MAX (ar_seg_size / 4 * 3) /* a segment with more live bytes is no compaction victim */

static uint32_t fair_segs; /* nsegs / nshards: what a shard may take back from others, and the largest entry (a division on every write cost store int ~3 ns) */
static uint16_t *ghosts[NOCOPY_MAX_SHARDS];
static uint32_t ghost_mask;
static uint64_t *expunges;
static nocopy_rebase_fn rebase;

void ar_room_adopt(uint32_t n, uint16_t *ghost_tabs, uint32_t slots_per, uint64_t *x, nocopy_rebase_fn fn)
{
	fair_segs = ar_nsegs / n;
	for (uint32_t i = 0; i < n; i++) {
		ghosts[i] = ghost_tabs + (size_t) i * slots_per;
	}
	ghost_mask = slots_per - 1;
	expunges = x;
	rebase = fn;
}

void ar_room_count_expunge(void) { __atomic_add_fetch(expunges, 1, __ATOMIC_RELAXED); }

/* ---- ghost ---- */

/* Direct-mapped: a newer eviction overwrites an older one in its cell. Index: Fibonacci mix below the shard's top 8 bits
 * (short keys' DJB hashes have few high bits); tag: the low bits (never 0). */
static uint16_t *ghost_cell(uint16_t *ghosts, uint64_t h) { return &ghosts[(uint32_t) (ar_fib_mix(h) >> 24) & ghost_mask]; }
static uint16_t ghost_tag(uint64_t h) { return (uint16_t) h | 1; }

#define GHOST_HOT 2 /* a returning key survives two merges unread */

uint8_t ar_room_admit(ar_shard *sh, uint64_t h)
{
	uint16_t *c = ghost_cell(ghosts[sh->idx], h);
	if (*c != ghost_tag(h)) {
		return 0;
	}
	*c = 0;
	sh->pol.ghost_hits++;
	return GHOST_HOT;
}

static void unlink_entry(nocopy_entry *e, void *arg)
{
	ar_shard *sh = arg;
	ar_slot *sp = ar_index_find(ix_of(sh), slot_for(e));
	if (sp) {
		ar_index_remove(ix_of(sh), sp);
	}
	die(sh, e);
}

/* Copies old forward into its segment's TTL bucket and repoints its slot. False if there is no room. */
static bool relocate(ar_shard *sh, nocopy_entry *old, uint8_t hot)
{
	nocopy_entry *n = ar_log_alloc(&sh->log, nocopy_entry_size(old), ar_segs[ar_seg_of(old)].bucket, old->expires, sh->last_seq, true);
	if (!n) {
		return false;
	}
	AR_HOOK("move.alloc");
	memcpy(n + 1, old + 1, nocopy_entry_size(old) - sizeof *old);
	n->h = old->h;
	n->expires = old->expires;
	n->created = old->created;
	n->seq = old->seq;
	n->klen = old->klen;
	n->koff = old->koff;
	n->flags = old->flags;
	n->user = old->user;
	n->hot = hot;
	ar_seg_deps_or(ar_seg_of(n), rebase(n, old));
	AR_HOOK("move.copied");
	ar_slot *sp = ar_index_find(ix_of(sh), slot_for(old));
	if (sp) {
		birth(sh, n);
		ar_index_put(ix_of(sh), sp, slot_for(n));
		AR_HOOK("move.linked");
	} else {
		ar_log_abort(&sh->log, n);
	}
	die_moved(sh, old); /* n takes its place, under the same hash */
	return true;
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
static uint32_t pick_oldest(ar_shard *sh, uint32_t *v, bool *spared, uint32_t K)
{
	uint32_t nv = 0, run = AR_NONE;
	*spared = false;
	AR_LOG_SEGS(&sh->log, s) {
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
		ar_log_requeue(run, sh->last_seq);
		*spared = true;
		return 0;
	}
	v[0] = run;
	return 1;
}

static int32_t seg_reads(uint32_t s) { return __atomic_load_n(&ar_segs[s].reads, __ATOMIC_RELAXED); }

/* The index of the least-read segment among c[from..n-1]. */
static uint32_t least_read(const uint32_t *c, uint32_t from, uint32_t n)
{
	uint32_t least = from;
	for (uint32_t i = from + 1; i < n; i++) {
		if (seg_reads(c[i]) < seg_reads(c[least])) {
			least = i;
		}
	}
	return least;
}

/* Eviction victims: of the PICK_N oldest, the oldest (so every segment is merged in time) and the MERGE_N - 1 least read. */
static uint32_t pick_victims(ar_shard *sh, uint32_t *v, bool *spared)
{
	uint32_t c[PICK_N];
	uint32_t nc = pick_oldest(sh, c, spared, PICK_N);
	uint32_t nv = nc < MERGE_N ? nc : MERGE_N;
	if (nv) {
		v[0] = c[0];
	}
	for (uint32_t n = 1; n < nv; n++) {
		uint32_t least = least_read(c, n, nc);
		v[n] = c[least];
		c[least] = c[n];
	}
	return nv;
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

/* e copied forward; out of segments, the victims already emptied give theirs first (each victim holds one TTL bucket, so one free segment is enough at a time). */
static bool move_one(ar_shard *sh, nocopy_entry *e, uint8_t hot)
{
	if (relocate(sh, e, hot)) {
		return true;
	}
	ar_log_reclaim(&sh->log);
	return relocate(sh, e, hot);
}

/* What a round may still move: every live entry when compacting, the most-read ones when evicting (levels >= cut, and spare bytes of the level below). */
typedef struct {
	size_t budget, spare;
	uint8_t cut;
	bool lossy;
} ar_moving;

/* Moves s's survivors forward. A moved copy keeps half its read count (aging), so it must be read again to stay on top.
 * False: no room for one (the budget is then spent). */
static bool move_survivors(ar_shard *sh, uint32_t s, ar_moving *mv)
{
	AR_SEG_FOREACH(s, e) {
		if (!mv->budget) {
			break;
		}
		if (!alive(e)) {
			continue;
		}
		uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED); /* readers may raise it meanwhile: cut is a hint */
		if (mv->lossy) {
			if (f < mv->cut - 1 || f == 0) {
				continue;
			}
			if (f < mv->cut) {
				if (nocopy_entry_size(e) > mv->spare) {
					continue;
				}
				mv->spare -= nocopy_entry_size(e);
			}
		}
		size_t size = nocopy_entry_size(e);
		if (size > mv->budget || !move_one(sh, e, f >> 1)) {
			mv->budget = 0;
			return false;
		}
		sh->pol.evacuated += size;
		mv->budget -= size;
	}
	return true;
}

/* Unlinks what s still holds: expired entries count as expired, the rest as evicted (ghost). *lost: the round was counted. */
static void evict_rest(ar_shard *sh, uint32_t s, bool *lost)
{
	AR_SEG_FOREACH(s, e) {
		if (!e->live) {
			continue;
		}
		if (nocopy_expired(e)) {
			ar_log_count_expired(&sh->log);
		} else {
			if (!*lost) {
				*lost = true;
				ar_room_count_expunge();
			}
			sh->pol.evicted++;
			*ghost_cell(ghosts[sh->idx], e->h) = ghost_tag(e->h);
		}
		unlink_entry(e, sh);
		AR_HOOK("merge.evicted");
	}
}

/* Victim by victim (a finished one may be given back, never walked again); out of room, the rest stays where it is. */
static void compact(ar_shard *sh, const uint32_t *v, uint32_t nv, size_t live)
{
	ar_moving mv = {.budget = live, .cut = 1};
	uint64_t before = sh->pol.evacuated;
	bool lost = false;
	for (uint32_t i = 0; i < nv; i++) {
		if (!move_survivors(sh, v[i], &mv)) {
			if (!i && sh->pol.evacuated == before) {
				evict_rest(sh, v[0], &lost); /* not one entry could move: the sparsest victim makes the room */
			}
			return;
		}
		AR_HOOK("merge.moved");
		evict_rest(sh, v[i], &lost);
	}
}

/* One round that evicts the oldest segments, keeping their most-read entries: for a full slot table, where compaction frees no slot.
 * Victims that readers pin die but stay allocated until unpinned; the next round moves on. */
static bool evict(ar_shard *sh)
{
	uint32_t v[MERGE_N];
	bool spared;
	uint32_t nv = pick_victims(sh, v, &spared);
	if (spared) {
		return true;
	}
	if (!nv) {
		return false;
	}
	AR_HOOK("merge.picked");
	ar_moving mv = {.budget = nv > 2 ? ar_seg_size : 0, .lossy = true, .cut = 1}; /* survivors fill at most one segment: frees at least one */
	if (mv.budget) {
		mv.cut = rank_cut(v, nv, mv.budget, &mv.spare);
	}
	bool lost = false;
	for (uint32_t i = 0; i < nv; i++) {
		move_survivors(sh, v[i], &mv);
		AR_HOOK("merge.moved");
		evict_rest(sh, v[i], &lost);
	}
	return true;
}

/* One round: compacts sparse segments losslessly, else evicts. False if there was nothing to merge. */
static bool compact_or_evict(ar_shard *sh)
{
	uint32_t v[MERGE_N];
	size_t live;
	uint32_t nv = pick_sparse(&sh->log, v, &live);
	if (!nv) {
		return evict(sh);
	}
	AR_HOOK("merge.picked");
	compact(sh, v, nv, live);
	return true;
}

static bool expire_step(ar_shard *sh, time_t now, int budget) { return ar_log_expire_step(&sh->log, now, budget, unlink_entry, sh); }

/* Below 15/16 of its share: may take segments back from a shard holding more. */
static bool below_share(const ar_shard *sh) { return sh->log.nown < fair_segs - fair_segs / 16; }

/* sh locked, below its share, and the pool is dry: the shard holding the most evicts once to give segments back.
 * Drops sh's lock meanwhile (one lock at a time). false: nobody holds more than its share. */
static __attribute__((cold, noinline)) bool rebalance(ar_shard *sh)
{
	uint32_t most = fair_segs;
	ar_shard *big = NULL;
	for (uint32_t i = 0; i <= ar_shard_mask; i++) {
		uint32_t n = __atomic_load_n(&ar_shards[i].log.nown, __ATOMIC_RELAXED);
		if (n > most) {
			most = n;
			big = &ar_shards[i];
		}
	}
	if (!big) {
		return false;
	}
	ar_shard_unlock(sh);
	AR_HOOK("rebalance.unlocked");
	ar_shard_lock(big);
	ar_log_reclaim(&big->log);
	if (big->log.nown > fair_segs && compact_or_evict(big)) {
		ar_log_reclaim(&big->log);
	}
	AR_HOOK("rebalance.merged");
	ar_shard_unlock(big);
	ar_shard_lock(sh);
	return true;
}

#define EXPIRE_PER_WRITE 64       /* entries each write's expiry step looks at */
#define EXPIRE_UNDER_PRESSURE 4096 /* the same when an allocation failed: catch up before copying live data */

/* Bounds each room-making loop: a round that frees nothing must not spin forever. */
static uint32_t max_rounds(void) { return 2 * fair_segs; }

static nocopy_entry *reclaim_alloc(ar_shard *sh, size_t total, int b, time_t exp)
{
	ar_log_reclaim(&sh->log);
	return ar_log_alloc(&sh->log, total, b, exp, sh->last_seq, false);
}

/* A block of total bytes, freeing room as needed, cheapest first: expired entries, dead segments, dead writers' unfinished
 * entries, segments other shards hold beyond their share, merges. NULL: no room.
 * May drop and retake the lock (rebalance): whatever the caller read of the shard before calling is stale afterwards. */
size_t nocopy_put_max(void) { return fair_segs ? (size_t) (fair_segs - 1) << ar_seg_shift : 0; }

nocopy_entry *ar_room_alloc(ar_shard *sh, size_t total, time_t exp, time_t now)
{
	if (total > nocopy_put_max()) {
		return NULL; /* bigger than a shard's share */
	}
	int b = ar_log_bucket(exp ? (exp > now ? exp - now : 1) : 0); /* exp == now is not bucket 0, which never expires */
	if (sh->log.exp_idle != now) { /* the common case: nothing left to expire this second */
		expire_step(sh, now, EXPIRE_PER_WRITE);
	}
	nocopy_entry *e = ar_log_alloc(&sh->log, total, b, exp, sh->last_seq, false);
	if (!e) {
		e = reclaim_alloc(sh, total, b, exp);
	}
	while (!e && expire_step(sh, now, EXPIRE_UNDER_PRESSURE)) {
		e = reclaim_alloc(sh, total, b, exp);
	}
	for (uint32_t tries = max_rounds(); !e && tries && below_share(sh) && rebalance(sh); tries--) {
		e = ar_log_alloc(&sh->log, total, b, exp, sh->last_seq, false);
	}
	for (uint32_t tries = max_rounds(); !e && tries && compact_or_evict(sh); tries--) {
		e = reclaim_alloc(sh, total, b, exp);
	}
	return e;
}

/* The slot table is full: evict whole segments' worth of entries until a slot is free. False if none could be. */
bool ar_room_slot(ar_shard *sh)
{
	for (uint32_t tries = max_rounds(); ar_index_full(ix_of(sh)) && tries && evict(sh); tries--) {}
	return !ar_index_full(ix_of(sh));
}
