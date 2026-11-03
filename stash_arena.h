/* stash arena: the shared-memory store without PHP. Used by the extension and by tests/c. */

#ifndef STASH_ARENA_H
#define STASH_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define STASH_MAX_WORKERS 1024 /* ponytail: more live processes/threads than this cannot claim a slot */
#define STASH_MAX_SEGS 256
#define STASH_SEG_WORDS (STASH_MAX_SEGS / 64)
#define STASH_TTL_BUCKETS 7
#define STASH_MAX_SHARDS 8
#define STASH_ALIGN(n) (((n) + 7) & ~(size_t) 7)

/* Block header; the user's payload (and the key bytes, at koff) follow in the same block. */
typedef struct stash_entry {
	uint64_t h;       /* key hash */
	size_t size;      /* whole block */
	time_t expires;   /* last second it is alive; 0: never */
	time_t created;
	uint64_t seq;     /* insertion order, for iterators */
	uint32_t klen;
	uint8_t koff;     /* key bytes at (char *) e + koff */
	uint8_t live;     /* published and not yet unlinked */
	uint8_t hot;      /* reads since written, log2-ish (stash_find); halved when merge moves it */
	uint8_t flags;    /* the user's */
} stash_entry;

/* low 32 bits: arena offset / 8, high 32: low 32 bits of the hash (home slot + probe filter); 0 empty */
typedef uint64_t stash_slot;

enum { SEG_FREE, SEG_OPEN, SEG_SEALED, SEG_DEAD, SEG_CONT };

/* Owned by one shard, touched only under its lock (stats read it racily). */
typedef struct {
	uint8_t state;
	uint32_t run;     /* head of a run: number of segments */
	uint32_t pending; /* allocated, not yet published */
	uint32_t gen;     /* bumped on every free: lets an expiry cursor notice reuse */
	size_t used;      /* bytes appended */
	size_t live;      /* bytes of live entries */
	time_t max_exp;   /* latest expiry of its entries; 0 in the ttl=0 bucket (never) */
	uint64_t born;    /* inserts counter when taken: merge order */
} stash_seg;

/* A shard owns its lock, a fixed slot table and a fixed slice of segments [lo, hi): writers of one shard never touch another's. */
typedef struct {
	uint64_t lock __attribute__((aligned(64))); /* holder's owner id; alone on its line */
	int32_t cur[STASH_TTL_BUCKETS] __attribute__((aligned(64))); /* open segment per TTL bucket, or -1 */
	uint32_t lo, hi, nfree, ndead;
	bool hint;          /* a segment died: worth a reclaim pass */
	time_t exp_idle;    /* second in which a full expiry pass found nothing */
	uint32_t exp_next, exp_gen; /* expiry cursor: segment, its gen, and offset of the next entry */
	size_t exp_off;
	uint64_t live_bytes, evacuated;
	uint64_t evicted, expired; /* entries unlinked by merge while unexpired / by their TTL */
	uint32_t nlive, cap; /* live slots, and the most the table may hold (3/4) */
	uint32_t mseq __attribute__((aligned(64))); /* odd while a delete shifts slots: a reader's miss is retried */
} stash_shard;

typedef struct {
	uint64_t owner;   /* owner id of the process (thread) holding the slot, 0 free */
	uint64_t bits[STASH_SEG_WORDS]; /* segments it pins */
	uint64_t hits, misses;
} __attribute__((aligned(64))) stash_worker;

typedef struct {
	size_t size, header_size;
	uint32_t nsegs, nshards, nworkers; /* nworkers: high-water mark of claimed slots */
	time_t start_time;
	uint64_t expunges;
	uint32_t lock_steals;
	uint64_t inserts __attribute__((aligned(64))); /* atomic */
	stash_shard shards[STASH_MAX_SHARDS];
	stash_worker workers[STASH_MAX_WORKERS];
	stash_seg segs[STASH_MAX_SEGS];
	char tables[] __attribute__((aligned(64)));
} stash_arena;

/* Per process (NTS) or per thread: what this reader has pinned. */
typedef struct {
	int slot;
	int pin_refs;     /* calls currently reading the arena */
	bool transient;   /* mine has bits that are not in keep */
	uint32_t rng;     /* xorshift state for the read counter */
	uint64_t mine[STASH_SEG_WORDS]; /* published as pinned */
	uint64_t keep[STASH_SEG_WORDS]; /* subset kept past the call: a zero-copy pointer escaped */
} stash_ctx;

typedef enum { PUT_OK, PUT_EXISTS, PUT_FULL } stash_put_result;

/* Read-only after stash_arena_create(); valid in forked children (same address). */
extern stash_arena *stash_a;
extern char *stash_seg_base;
extern unsigned stash_seg_shift;
extern uint32_t stash_shard_mask, stash_tab_mask;
extern stash_slot *stash_tabs[STASH_MAX_SHARDS];
extern uint64_t stash_self; /* this process's owner id */

/* Supplied by the user: copy src's payload and key bytes into dst (same size and koff; header already set). Shard locked. */
void stash_relocate(stash_entry *dst, const stash_entry *src);

/* Maps size bytes (rounded to the arena layout). hint: entries the slot tables should hold. NULL, or an error;
 * STASH_TOO_SMALL: size cannot hold the slot tables (*need bytes) plus a few segments. */
extern const char STASH_TOO_SMALL[];
const char *stash_arena_create(size_t size, uint64_t hint, size_t *need);
void stash_arena_destroy(void);

bool stash_claim(stash_ctx *c);   /* false: no free worker slot */
void stash_release(stash_ctx *c); /* end of this process/thread */
void stash_after_fork(stash_ctx *c);
void stash_keep_release(stash_ctx *c);

/* ---- writes: lock the key's shard, alloc, fill (header + payload), commit, unlock ---- */
void stash_lock_wait(stash_shard *sh);
stash_entry *stash_alloc(stash_shard *sh, size_t total, time_t exp, time_t now);
stash_put_result stash_commit(stash_shard *sh, stash_entry *e, bool add_only);
void stash_abort(stash_shard *sh, stash_entry *e);
stash_entry *stash_lookup(stash_shard *sh, uint64_t h, const char *k, uint32_t klen); /* locked; may be expired */
bool stash_remove(stash_shard *sh, uint64_t h, const char *k, uint32_t klen);       /* locked; true if it was live */
void stash_clear(void);

/* ---- lock-free scan: next pinned unexpired entry at or after (*sh, *i); start at 0, 0. Between borrow_begin/end. ---- */
stash_entry *stash_scan_next(stash_ctx *c, uint32_t *sh, uint32_t *i);

typedef struct {
	uint64_t used, entries, evacuated, hits, misses, evicted, expired, expunges, lock_steals, slots;
	uint32_t segments, free_segments, dead_segments, pinned_segments, shards;
} stash_stats_t;
void stash_stats(stash_stats_t *s);
/* A shard's free (or dead) segments as runs, for apcu_sma_info: up to max (offset from the slice start, bytes) pairs; returns how many. */
uint32_t stash_free_runs(uint32_t shard, size_t *off, size_t *len, uint32_t max);

/* ---- inline read path ---- */

static inline const char *stash_key(const stash_entry *e) { return (const char *) e + e->koff; }
static inline uint32_t stash_seg_of(const void *p) { return (uint32_t) (((const char *) p - stash_seg_base) >> stash_seg_shift); }
static inline stash_shard *stash_shard_of(uint64_t h)
{
	/* Fibonacci mix of the high bits: the low ones pick the slot */
	return &stash_a->shards[(uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 56) & stash_shard_mask];
}
static inline stash_entry *stash_slot_entry(stash_slot s) { return (stash_entry *) ((char *) stash_a + ((size_t) (uint32_t) s << 3)); }
/* Like APCu: alive through the whole second `expires`, so ttl=1 lives 1-2 s, not 0-1 s. */
static inline bool stash_expired(const stash_entry *e) { return e->expires && e->expires < time(NULL); }

static inline void stash_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
}

static inline void stash_lock(stash_shard *sh)
{
	uint64_t z = 0;
	if (!__atomic_compare_exchange_n(&sh->lock, &z, stash_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		stash_lock_wait(sh);
	}
}

static inline void stash_unlock(stash_shard *sh) { __atomic_store_n(&sh->lock, 0, __ATOMIC_RELEASE); }

/* Publish "I may touch this segment" before dereferencing anything in it.
 * True if the pin is new: the caller must then re-check that what it found is still linked. */
static inline bool stash_hold(stash_ctx *c, const void *p)
{
	uint32_t s = stash_seg_of(p), w = s >> 6;
	uint64_t b = (uint64_t) 1 << (s & 63);
	if (c->mine[w] & b) {
		return false;
	}
	c->mine[w] |= b;
	c->transient = true;
	__atomic_store_n(&stash_a->workers[c->slot].bits[w], c->mine[w], __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return true;
}

/* A zero-copy pointer into p's segment is leaving the caller: keep it pinned until stash_keep_release(). */
static inline void stash_keep(stash_ctx *c, const void *p)
{
	uint32_t s = stash_seg_of(p);
	c->keep[s >> 6] |= (uint64_t) 1 << (s & 63);
}

static inline void stash_drop_transient(stash_ctx *c)
{
	if (c->slot < 0 || !c->transient) {
		return;
	}
	for (int w = 0; w < STASH_SEG_WORDS; w++) {
		if (c->mine[w] & ~c->keep[w]) {
			c->mine[w] &= c->keep[w];
			__atomic_store_n(&stash_a->workers[c->slot].bits[w], c->mine[w], __ATOMIC_SEQ_CST);
		}
	}
	c->transient = false;
}

static inline void stash_borrow_begin(stash_ctx *c) { c->pin_refs++; }
static inline void stash_borrow_end(stash_ctx *c)
{
	if (--c->pin_refs == 0) {
		stash_drop_transient(c);
	}
}

#define STASH_HOT_MAX 15

static inline uint32_t stash_rand(stash_ctx *c)
{
	uint32_t x = c->rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return c->rng = x;
}

/* Lock-free; between borrow_begin/end. Forced inline: gcc outlined it once fetch() got more callers (+9 ns copy fetch). The entry's segment is pinned before it is touched.
 * A miss counts only if no delete shifted slots meanwhile (mseq): one may move the key backwards past the probe. */
static inline __attribute__((always_inline)) stash_entry *stash_find(stash_ctx *c, uint64_t h, const char *k, uint32_t klen)
{
	stash_shard *sh = stash_shard_of(h);
	const stash_slot *t = stash_tabs[sh - stash_a->shards];
	uint32_t m = stash_tab_mask, tag = (uint32_t) h;
	for (;;) {
	retry:;
		uint32_t seq = __atomic_load_n(&sh->mseq, __ATOMIC_ACQUIRE);
		for (uint32_t n = 0, i = h & m; n <= m; n++, i = (i + 1) & m) {
			stash_slot s = __atomic_load_n(&t[i], __ATOMIC_ACQUIRE);
			if (!s) {
				break;
			}
			if ((uint32_t) (s >> 32) != tag) {
				continue;
			}
			stash_entry *e = stash_slot_entry(s);
			if (stash_hold(c, e) && __atomic_load_n(&t[i], __ATOMIC_ACQUIRE) != s) {
				goto retry; /* unlinked while we were pinning: the memory may be reused */
			}
			if (e->h == h && e->klen == klen && !memcmp(stash_key(e), k, klen)) {
				if (stash_expired(e)) {
					return NULL;
				}
				uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
				if (f < STASH_HOT_MAX && (!f || !(stash_rand(c) & ((1u << f) - 1)))) {
					__atomic_store_n(&e->hot, f + 1, __ATOMIC_RELAXED); /* p = 1/2^f: ~log2(reads) writes, the line stays shared */
				}
				return e;
			}
		}
		/* no fence: the slot loads above are acquire loads, so this one cannot move before them */
		if (!(seq & 1) && __atomic_load_n(&sh->mseq, __ATOMIC_RELAXED) == seq) {
			return NULL;
		}
		stash_cpu_relax();
	}
}

#endif
