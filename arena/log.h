/* nocopy arena: a shard's log (Segcache). Entries are appended to one open segment per TTL bucket; segments are sealed when
 * full, die when nothing in them is live or pending, and go back to the pool once no worker pins them. The log never frees
 * room on its own: allocation fails, and its caller expires, merges or evicts. Its shard's lock is held for everything here. */

#ifndef NOCOPY_ARENA_LOG_H
#define NOCOPY_ARENA_LOG_H

#include "entry.h"
#include "pool.h"

#define NOCOPY_TTL_BUCKETS 7 /* ttl 0, < 8 s, < 64 s, < 512 s, < 4096 s, < 32768 s, longer */

enum { SEG_FREE, SEG_OPEN, SEG_SEALED, SEG_DEAD, SEG_CONT };

/* Per segment, owned by the shard holding it. Zeroed when it goes back to the pool. Stats read it racily. */
typedef struct {
	uint8_t state;
	uint8_t bucket;   /* TTL bucket of an open or sealed single segment */
	uint32_t run;     /* head of a run: number of segments */
	uint32_t pending; /* allocated, not yet published */
	int32_t reads;    /* the policy's: estimated reads of its live entries (sum of 2^hot - 1); readers add racily */
	size_t used;      /* bytes appended */
	size_t live;      /* bytes of live entries */
	time_t max_exp;   /* latest expiry of its entries; 0 in the ttl=0 bucket (never) */
	uint64_t born;    /* merge order: its shard's last seq when taken, or when sent to the back */
} ar_seg;

AR_HIDDEN extern ar_seg *ar_segs;

/* A shard's log, in the shard. */
typedef struct {
	int32_t cur[NOCOPY_TTL_BUCKETS]; /* open segment per TTL bucket, or -1 */
	uint32_t nown, ndead; /* nown: read racily by other shards (rebalance) */
	uint8_t tenant;       /* its id in the pool: shard index + 1 */
	bool hint;            /* a segment died: worth a reclaim pass */
	uint32_t home;        /* where its pool searches start: balanced shards stay contiguous */
	time_t exp_idle;      /* second in which a full expiry pass found nothing */
	uint32_t exp_next, exp_gen; /* expiry cursor: segment, its gen, and offset of the next entry */
	size_t exp_off;
	uint64_t live_bytes;
	uint64_t expired;     /* entries unlinked because their TTL passed */
	uint64_t own[NOCOPY_SEG_WORDS]; /* its segments; the pool's owner[] is the truth, this the index */
} ar_log;

/* zero-filled memory. home: (interleaved, shards' ar_segs share cache lines: parallel stores -50%) */
void ar_log_init(ar_log *lg, uint8_t tenant, uint32_t home);
void ar_log_adopt(ar_seg *segs, uint32_t nlogs); /* nlogs: segments a plain allocation leaves free, one per log */

static inline bool ar_log_owns(const ar_log *lg, uint32_t s) { return ar_segs_has(lg->own, s); }

/* The first segment >= s that lg holds, or AR_NONE. */
static inline uint32_t ar_log_next(const ar_log *lg, uint32_t s)
{
	if (s >= ar_nsegs) {
		return AR_NONE;
	}
	uint32_t w = s >> 6;
	uint64_t b = lg->own[w] & (~(uint64_t) 0 << (s & 63));
	while (!b) {
		if (++w == ar_seg_words) {
			return AR_NONE;
		}
		b = lg->own[w];
	}
	return w * 64 + (uint32_t) __builtin_ctzll(b);
}
#define AR_LOG_SEGS(lg, s) for (uint32_t s = ar_log_next(lg, 0); s != AR_NONE; s = ar_log_next(lg, s + 1))

/* The entries of segment s. A walk may run past unlinks that make s DEAD: only ar_log_reclaim frees it, under this lock. */
#define AR_SEG_FOREACH(s, e) \
	for (nocopy_entry *e = (nocopy_entry *) ar_seg_start(s), *e##_end = (nocopy_entry *) (ar_seg_start(s) + ar_segs[s].used); \
	     e < e##_end; e = (nocopy_entry *) ((char *) e + e->size))

static inline void ar_seg_set(uint32_t s, uint8_t st) { __atomic_store_n(&ar_segs[s].state, st, __ATOMIC_RELEASE); }

/* The only way SEALED becomes DEAD: call it after any change of live or pending. */
static inline void ar_log_check(ar_log *lg, uint32_t s)
{
	ar_seg *g = &ar_segs[s];
	if (g->state == SEG_SEALED && !g->live && !g->pending) {
		ar_seg_set(s, SEG_DEAD);
		lg->ndead++;
		lg->hint = true;
	}
}

int ar_log_bucket(time_t ttl);

/* Sets the block's size before used covers it: a repair walk never meets a header without a size. Sizes are multiples of 8. */
static inline __attribute__((always_inline)) nocopy_entry *ar_log_carve(uint32_t s, size_t total, time_t exp)
{
	ar_seg *g = &ar_segs[s];
	nocopy_entry *e = (nocopy_entry *) (ar_seg_start(s) + g->used);
	e->size = total;
	e->live = false;
	e->hot = 0;
	g->used += total;
	g->pending++;
	if (exp > g->max_exp) {
		g->max_exp = exp;
	}
	return e;
}

/* A new segment (or run) from the pool. born: its merge order. reserve: may take the last free segment kept for merges. */
nocopy_entry *ar_log_alloc_new(ar_log *lg, size_t total, int bucket, time_t exp, uint64_t born, bool reserve);

/* A pending block of total bytes in the bucket's open segment, or a new one; NULL if the pool has none to give. */
static inline __attribute__((always_inline)) nocopy_entry *ar_log_alloc(ar_log *lg, size_t total, int bucket, time_t exp, uint64_t born, bool reserve)
{
	int32_t cur = lg->cur[bucket];
	if (total <= ar_seg_size / 2 && cur >= 0 && ar_segs[cur].used + total <= ar_seg_size) {
		return ar_log_carve((uint32_t) cur, total, exp);
	}
	return ar_log_alloc_new(lg, total, bucket, exp, born, reserve);
}

/* pending -> live */
static inline __attribute__((always_inline)) void ar_log_born(ar_log *lg, nocopy_entry *e)
{
	ar_seg *g = &ar_segs[ar_seg_of(e)];
	e->live = true;
	g->pending--;
	g->live += e->size;
	lg->live_bytes += e->size;
}

/* live -> dead (its slot already gone or repointed) */
static inline __attribute__((always_inline)) void ar_log_died(ar_log *lg, nocopy_entry *e)
{
	uint32_t s = ar_seg_of(e);
	e->live = false;
	ar_segs[s].live -= e->size;
	lg->live_bytes -= e->size;
	ar_log_check(lg, s);
}

/* pending -> gone: allocated, never published */
static inline void ar_log_abort(ar_log *lg, nocopy_entry *e)
{
	uint32_t s = ar_seg_of(e);
	ar_segs[s].pending--;
	ar_log_check(lg, s);
}

void ar_log_seal(ar_log *lg, int bucket); /* no more writes go to its open segment */
void ar_log_seal_all(ar_log *lg);
/* Gives lg's dead segments that no live worker pins back to the pool. */
void ar_log_reclaim(ar_log *lg);
static inline void ar_log_requeue(uint32_t s, uint64_t born) { ar_segs[s].born = born; }
static inline void ar_log_count_expired(ar_log *lg) { lg->expired++; }

/* Unlinks (unlink(e, arg): slot and ar_log_died) the live entries of lg's segments whose entries have all expired,
 * looking at most `budget` entries. True if it unlinked anything. A full pass with nothing to do idles until the next second. */
bool ar_log_expire_step(ar_log *lg, time_t now, int budget, void (*unlink)(nocopy_entry *e, void *arg), void *arg);

/* Repair after a holder died, in three steps. Rebuild: segments from the pool's owner[] (a run taken or freed halfway,
 * an open segment never installed), live bytes from linked(e), pending 0. Then ar_log_hold each entry still being filled
 * outside the lock. Settle: dead counts, cursor, reclaim. */
void ar_log_rebuild(ar_log *lg, bool (*linked)(const nocopy_entry *e, void *arg), void *arg);
static inline void ar_log_hold(const nocopy_entry *e) { ar_segs[ar_seg_of(e)].pending++; }
void ar_log_settle(ar_log *lg);

#endif
