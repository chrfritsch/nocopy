/* nocopy arena: the shared-memory store without PHP. Used by the extension and by tests/c. */

#ifndef NOCOPY_ARENA_H
#define NOCOPY_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define NOCOPY_MAX_WORKERS 1024 /* ponytail: more live processes/threads than this cannot claim a slot */
#define NOCOPY_MAX_SEGS 8192
#define NOCOPY_SEG_MAX_SHIFT 22 /* segments grow to 4 MB, then their number: an eviction round moves up to one */
#define NOCOPY_SEG_WORDS (NOCOPY_MAX_SEGS / 64)
#define NOCOPY_TTL_BUCKETS 7
#define NOCOPY_MAX_SHARDS 8
#define NOCOPY_LEASES 32 /* per shard: concurrent single-flight computes */
#define NOCOPY_ALIGN(n) (((n) + 7) & ~(size_t) 7)
#define NOCOPY_INLINE_PUT 2048 /* blocks up to this size are filled under the lock: one lock round trip per write */

/* Block header; the user's payload (and the key bytes, at koff) follow in the same block. */
typedef struct nocopy_entry {
	uint64_t h;       /* key hash */
	size_t size;      /* whole block */
	time_t expires;   /* last second it is alive; 0: never */
	uint64_t seq;     /* insertion time, us << 3 | shard; strictly increasing per shard: iterator order without a shared counter */
	uint32_t created; /* unix time: 32 bits keep the header at 48 bytes (good until 2106) */
	uint32_t klen;
	uint8_t koff;     /* key bytes at (char *) e + koff */
	uint8_t live;     /* published and not yet unlinked */
	uint8_t hot;      /* reads since written, log2-ish (nocopy_find); halved when merge moves it */
	uint8_t flags;    /* the user's */
	uint32_t user;    /* the user's; kept when merge moves the entry */
} nocopy_entry;

_Static_assert(sizeof(nocopy_entry) == 48, "nocopy_entry grew");

/* low 32 bits: arena offset / 8, high 32: nocopy_slot_tag(hash) (its low bits are the home slot; probe filter); 0 empty */
typedef uint64_t nocopy_slot;

enum { SEG_FREE, SEG_OPEN, SEG_SEALED, SEG_DEAD, SEG_CONT };

/* Owned by one shard, touched only under its lock (stats read it racily). */
typedef struct {
	uint8_t state;
	uint8_t bucket;   /* TTL bucket of an open or sealed single segment */
	uint32_t run;     /* head of a run: number of segments */
	uint32_t pending; /* allocated, not yet published */
	uint32_t gen;     /* bumped on every free: lets an expiry cursor notice reuse */
	int32_t reads;    /* estimated reads of its live entries (sum of 2^hot - 1); readers add racily */
	size_t used;      /* bytes appended */
	size_t live;      /* bytes of live entries */
	time_t max_exp;   /* latest expiry of its entries; 0 in the ttl=0 bucket (never) */
	uint64_t born;    /* its shard's last seq when taken: merge order */
} nocopy_seg;

/* Who computes a key hash right now (single-flight); free when owner is 0 or until has passed. */
typedef struct {
	uint64_t h, owner;
	time_t until;
	int32_t slot;
} nocopy_lease;

/* A shard owns its lock, a fixed slot table and the segments it took from the pool (own): writers of one shard never touch another's. */
typedef struct {
	uint64_t lock __attribute__((aligned(64))); /* holder's owner id; alone on its line */
	int32_t cur[NOCOPY_TTL_BUCKETS] __attribute__((aligned(64))); /* open segment per TTL bucket, or -1 */
	uint32_t nown, ndead; /* nown: read racily by other shards (rebalance) */
	uint32_t idx;       /* own index: sh - shards would divide by sizeof(nocopy_shard) */
	uint64_t last_seq, inserts; /* a global atomic counter here cost 9x at 8 parallel writers */
	bool hint;          /* a segment died: worth a reclaim pass */
	time_t exp_idle;    /* second in which a full expiry pass found nothing */
	uint32_t exp_next, exp_gen; /* expiry cursor: segment, its gen, and offset of the next entry */
	size_t exp_off;
	uint64_t live_bytes, evacuated;
	uint64_t evicted, expired; /* entries unlinked by merge while unexpired / by their TTL */
	uint64_t ghost_hits;        /* new keys that were evicted recently: they start hot */
	uint32_t nlive, cap; /* live slots, and the most the table may hold (3/4) */
	time_t wip_checked;  /* last look for dead writers' unfinished entries */
	nocopy_lease leases[NOCOPY_LEASES];
	uint64_t own[NOCOPY_SEG_WORDS]; /* its segments; owner[] is the truth, this the index. Here: off the write path's lines */
	uint32_t mseq __attribute__((aligned(64))); /* odd while a delete shifts slots: a reader's miss is retried */
} nocopy_shard;

typedef struct {
	uint64_t owner;   /* owner id of the process (thread) holding the slot, 0 free */
	uint64_t hits, misses;
	uint32_t wip;     /* offset/8 of the entry it fills outside the shard lock; 0 none. Changes only under that shard's lock */
} __attribute__((aligned(64))) nocopy_worker;

typedef struct {
	uint64_t magic, abi; /* named arenas: magic is stored last, once the rest is set up */
	uintptr_t base;      /* the address every process maps it at */
	size_t size, header_size;
	unsigned seg_shift;
	uint32_t tab_per;    /* slots per shard */
	uint32_t nsegs, nshards, nworkers; /* nworkers: high-water mark of claimed slots */
	size_t segs_off, owner_off, pins_off; /* behind the slot tables, sized by the segment count */
	uint32_t pin_stride; /* words per worker's pin bitmap, a multiple of 8 (own cache lines) */
	time_t start_time;
	uint64_t expunges;
	uint32_t lock_steals;
	uint32_t unpin_gen __attribute__((aligned(64))); /* bumped when a dead segment is still pinned: readers drop their idle pins */
	nocopy_shard shards[NOCOPY_MAX_SHARDS];
	nocopy_worker workers[NOCOPY_MAX_WORKERS];
	char user[256] __attribute__((aligned(64))); /* the user's shared state, zeroed at creation (nocopy_php.h: nocopy_shared) */
	char tables[] __attribute__((aligned(64)));
} nocopy_arena;

/* Per process (NTS) or per thread: what this reader has pinned. */
typedef struct {
	int slot;
	int pin_refs;     /* calls currently reading the arena */
	bool transient;   /* mine has bits that are not in keep */
	uint32_t rng;     /* xorshift state for the read counter */
	uint32_t unpin_seen; /* nocopy_a->unpin_gen when the idle pins were last dropped */
	uint32_t idle;       /* pins taken since then (upper bound) */
	uint32_t nkeep;      /* segments in keep */
	uint64_t mine[NOCOPY_SEG_WORDS]; /* published as pinned */
	uint64_t keep[NOCOPY_SEG_WORDS]; /* subset kept past the call: a zero-copy pointer escaped */
} nocopy_ctx;

typedef enum { PUT_OK, PUT_EXISTS, PUT_FULL } nocopy_put_result;

/* A write between nocopy_put_begin() and nocopy_put_commit(). */
typedef struct {
	nocopy_ctx *c;
	nocopy_shard *sh;
	nocopy_entry *e;
	bool locked; /* small block: filled under the shard lock */
} nocopy_put;

/* Read-only after nocopy_arena_create(); valid in forked children (same address). Hidden: no GOT load on the read path. */
#pragma GCC visibility push(hidden)
extern nocopy_arena *nocopy_a;
extern char *nocopy_seg_base;
extern unsigned nocopy_seg_shift;
extern uint32_t nocopy_seg_words; /* bitmap words in use: (nsegs + 63) / 64 */
extern nocopy_seg *nocopy_segs;     /* per segment, owned by one shard */
extern uint8_t *nocopy_seg_owner;   /* per segment: shard index + 1; 0: in the pool (free, zeroed) */
extern uint64_t *nocopy_pins;       /* per worker slot: segments it pins (pin_stride words) */
extern uint32_t nocopy_pin_stride;
static inline uint64_t *nocopy_wbits(int slot) { return nocopy_pins + (size_t) slot * nocopy_pin_stride; }
extern uint32_t nocopy_shard_mask, nocopy_tab_mask;
extern nocopy_slot *nocopy_tabs[NOCOPY_MAX_SHARDS];
extern uint64_t nocopy_self; /* this process's owner id */
#pragma GCC visibility pop

/* Supplied by the user: e is a byte copy of `from`'s block (merge moved it); repoint e's pointers into its own block. Shard locked, must not allocate. */
void nocopy_rebase(nocopy_entry *e, const nocopy_entry *from);

extern const char NOCOPY_TOO_SMALL[]; /* size cannot hold the slot tables (*need bytes) plus a few segments */

typedef struct {
	char name[64];  /* the POSIX shm object: base.<abi> */
	char other[64]; /* another build's arena under the same base, still in use by its processes; "" none */
} nocopy_named;

/* Creates or attaches to the arena any process opening base with the same abi (the user's layout id) shares, at the same address.
 * hint: entries the slot tables should hold. An attacher gets the creator's size. Unused arenas of other builds under base are removed. NULL, or an error (then no arena). */
const char *nocopy_arena_open(const char *base, size_t size, uint64_t hint, uint64_t abi, size_t *need, nocopy_named *out);
void nocopy_arena_unhold(void);  /* the Valkey listener maps the arena without counting as its user */
bool nocopy_arena_current(void); /* false: the named arena's name was removed or names another object now */
void nocopy_arena_destroy(void);

bool nocopy_claim(nocopy_ctx *c);   /* false: no free worker slot */
void nocopy_release(nocopy_ctx *c); /* end of this process/thread */
void nocopy_after_fork(nocopy_ctx *c);
void nocopy_keep_release(nocopy_ctx *c);
void nocopy_request_end(nocopy_ctx *c); /* every pin goes, also of calls a bailout left open */

/* ---- writes ---- */

/* Locks h's shard and allocates a size-byte block; NULL (unlocked) if it does not fit. The header is set (flags and user 0):
 * the caller writes the key's klen bytes at koff and its payload, then commits. Blocks over NOCOPY_INLINE_PUT are filled
 * with the lock dropped; a writer that dies meanwhile has its block rolled back. ttl 0: never expires. */
nocopy_entry *nocopy_put_begin(nocopy_put *p, nocopy_ctx *c, uint64_t h, uint32_t klen, uint8_t koff, size_t size, time_t ttl);
/* Publishes p->e under its key (add_only: not over a live entry) and unlocks. */
nocopy_put_result nocopy_put_commit(nocopy_put *p, bool add_only);

/* ---- read-modify-write: the caller holds nocopy_lock(nocopy_shard_of(h)) ---- */
void nocopy_lock_wait(nocopy_shard *sh);
nocopy_entry *nocopy_lookup(nocopy_shard *sh, uint64_t h, const char *k, uint32_t klen); /* locked; may be expired */
bool nocopy_remove(nocopy_shard *sh, uint64_t h, const char *k, uint32_t klen);       /* locked; true if it was live */
void nocopy_clear(void);

/* Single-flight per key hash, under the hash's shard lock. A dead or expired holder is taken over. */
enum { NOCOPY_LEASE_TAKEN, NOCOPY_LEASE_HELD, NOCOPY_LEASE_MINE, NOCOPY_LEASE_FULL };
int nocopy_lease_take(uint64_t h, int32_t slot, time_t now, time_t ttl);
void nocopy_lease_drop(uint64_t h, int32_t slot); /* only if this slot still holds it */

typedef struct {
	uint64_t used, entries, evacuated, hits, misses, evicted, expired, ghost_hits, expunges, lock_steals, slots, leases, inserts;
	uint64_t segment_size;
	uint32_t segments, free_segments, dead_segments, pinned_segments, shards;
	time_t start_time;
} nocopy_stats_t;
void nocopy_stats(nocopy_stats_t *s);

/* Lock-free: fn(e) for each live, unexpired entry, which stays pinned until borrow_end. Between borrow_begin/end.
 * ponytail: an entry a concurrent delete shifts backwards may be missed; a snapshot under all shard locks if that matters. */
void nocopy_each(nocopy_ctx *c, void (*fn)(const nocopy_entry *e, void *arg), void *arg);

/* ---- inline read path ---- */

static inline const char *nocopy_key(const nocopy_entry *e) { return (const char *) e + e->koff; }
/* Segment bitmaps of NOCOPY_SEG_WORDS words: pins, ownership, reclaim's sets. */
static inline uint64_t nocopy_segbit(uint32_t s) { return (uint64_t) 1 << (s & 63); }
static inline bool nocopy_segs_has(const uint64_t *m, uint32_t s) { return m[s >> 6] >> (s & 63) & 1; }
static inline void nocopy_segs_add(uint64_t *m, uint32_t s) { m[s >> 6] |= nocopy_segbit(s); }
static inline void nocopy_segs_del(uint64_t *m, uint32_t s) { m[s >> 6] &= ~nocopy_segbit(s); }
static inline uint32_t nocopy_seg_of(const void *p) { return (uint32_t) (((const char *) p - nocopy_seg_base) >> nocopy_seg_shift); }
/* Fibonacci mix, top bits: the shard */
static inline uint32_t nocopy_shard_idx(uint64_t h) { return (uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 56) & nocopy_shard_mask; }
static inline nocopy_shard *nocopy_shard_of(uint64_t h) { return &nocopy_a->shards[nocopy_shard_idx(h)]; }
/* Mixed: raw DJB low bits of `prefix.N` keys built probe runs of hundreds. Own multiplier keeps it independent of the shard; tests/018 mirrors it. */
static inline uint32_t nocopy_slot_tag(uint64_t h) { return (uint32_t) ((h * 0xC2B2AE3D27D4EB4FULL) >> 32); }
static inline nocopy_entry *nocopy_slot_entry(nocopy_slot s) { return (nocopy_entry *) ((char *) nocopy_a + ((size_t) (uint32_t) s << 3)); }
/* The coarse clock is a tick behind at most (1-4 ms) and 3x cheaper than CLOCK_REALTIME (3.5 vs 11.2 ns): TTLs are seconds. */
static inline void nocopy_clock(struct timespec *ts)
{
#ifdef CLOCK_REALTIME_COARSE
	clock_gettime(CLOCK_REALTIME_COARSE, ts);
#else
	clock_gettime(CLOCK_REALTIME, ts);
#endif
}
static inline time_t nocopy_now(void)
{
	struct timespec ts;
	nocopy_clock(&ts);
	return ts.tv_sec;
}
#define NOCOPY_SEQ_SHARD_BITS 3 /* seq: microseconds << 3 | shard, unique across shards (a segment changes shard, copy mode keys on address + seq) */
_Static_assert(NOCOPY_MAX_SHARDS <= 1 << NOCOPY_SEQ_SHARD_BITS, "shard index must fit seq's low bits");
/* Like APCu: alive through the whole second `expires`, so ttl=1 lives 1-2 s, not 0-1 s. */
static inline bool nocopy_expired(const nocopy_entry *e) { return e->expires && e->expires < nocopy_now(); }

static inline void nocopy_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
}

static inline void nocopy_lock(nocopy_shard *sh)
{
	uint64_t z = 0;
	if (!__atomic_compare_exchange_n(&sh->lock, &z, nocopy_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		nocopy_lock_wait(sh);
	}
}

static inline void nocopy_unlock(nocopy_shard *sh) { __atomic_store_n(&sh->lock, 0, __ATOMIC_RELEASE); }

/* Publish "I may touch this segment" before dereferencing anything in it.
 * True if the pin is new: the caller must then re-check that what it found is still linked. */
static inline bool nocopy_hold(nocopy_ctx *c, const void *p)
{
	uint32_t s = nocopy_seg_of(p), w = s >> 6;
	uint64_t b = nocopy_segbit(s);
	if (c->mine[w] & b) {
		return false;
	}
	c->mine[w] |= b;
	c->transient = true;
	c->idle++;
	__atomic_store_n(&nocopy_wbits(c->slot)[w], c->mine[w], __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return true;
}

/* A zero-copy pointer into p's segment is leaving the caller: keep it pinned until nocopy_keep_release().
 * True once more than a quarter of the segments are kept: kept pins block reclaim for everyone, so the caller's further fetches
 * should copy. Not gated on pressure: a writer first meets a pinned dead segment when nearly all are kept. */
static inline bool nocopy_keep(nocopy_ctx *c, const void *p)
{
	uint32_t s = nocopy_seg_of(p);
	if (c->keep[s >> 6] & nocopy_segbit(s)) {
		return false;
	}
	nocopy_segs_add(c->keep, s);
	return ++c->nkeep > nocopy_a->nsegs / 4;
}

static inline void nocopy_drop_transient(nocopy_ctx *c)
{
	if (c->slot < 0 || !c->transient) {
		return;
	}
	for (uint32_t w = 0; w < nocopy_seg_words; w++) {
		if (c->mine[w] & ~c->keep[w]) {
			c->mine[w] &= c->keep[w];
			__atomic_store_n(&nocopy_wbits(c->slot)[w], c->mine[w], __ATOMIC_SEQ_CST);
		}
	}
	c->transient = false;
	c->idle = 0;
}

/* Idle pins outlive the call (re-pinning costs a full fence per call); dropped once a writer asks, past nsegs/16, at request end.
 * ponytail: a process idle between calls holds up to nsegs/16 segments; revoking them would need the per-call fence back. */
static inline void nocopy_drop_if_asked(nocopy_ctx *c)
{
	uint32_t g = __atomic_load_n(&nocopy_a->unpin_gen, __ATOMIC_RELAXED);
	if (c->transient && (g != c->unpin_seen || c->idle > (nocopy_a->nsegs >> 4))) {
		c->unpin_seen = g;
		nocopy_drop_transient(c);
	}
}

static inline void nocopy_borrow_begin(nocopy_ctx *c) { c->pin_refs++; }
static inline void nocopy_borrow_end(nocopy_ctx *c)
{
	if (--c->pin_refs == 0) {
		nocopy_drop_if_asked(c);
	}
}

/* About to wait (FrankenPHP worker between requests): drop every pin not kept, idle or not. */
static inline void nocopy_drop_idle(nocopy_ctx *c)
{
	c->transient = c->slot >= 0;
	nocopy_drop_transient(c);
}

/* A lookup's outcome in this worker's counters (nocopy_stats). Single writer per slot: no RMW. */
static inline void nocopy_count(nocopy_ctx *c, bool hit)
{
	uint64_t *n = hit ? &nocopy_a->workers[c->slot].hits : &nocopy_a->workers[c->slot].misses;
	__atomic_store_n(n, *n + 1, __ATOMIC_RELAXED);
}

#define NOCOPY_HOT_MAX 15

static inline uint32_t nocopy_rand(nocopy_ctx *c)
{
	uint32_t x = c->rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return c->rng = x;
}

/* Equal n bytes; keys up to 16 bytes without a memcmp call (two overlapping loads). */
static inline __attribute__((always_inline)) bool nocopy_key_eq(const char *a, const char *b, uint32_t n)
{
	uint64_t x, y, p, q;
	if (n >= 8 && n <= 16) {
		memcpy(&x, a, 8); memcpy(&y, b, 8); memcpy(&p, a + n - 8, 8); memcpy(&q, b + n - 8, 8);
		return ((x ^ y) | (p ^ q)) == 0;
	}
	if (n >= 4 && n < 8) {
		uint32_t u, v, s, t;
		memcpy(&u, a, 4); memcpy(&v, b, 4); memcpy(&s, a + n - 4, 4); memcpy(&t, b + n - 4, 4);
		return ((u ^ v) | (s ^ t)) == 0;
	}
	if (n < 4) {
		return !n || (a[0] == b[0] && a[n >> 1] == b[n >> 1] && a[n - 1] == b[n - 1]);
	}
	return !memcmp(a, b, n);
}

/* An expired entry that a lease waiter may still be served: expired less than its TTL ago. */
static inline bool nocopy_stale_ok(const nocopy_entry *e) { return e->expires && (time_t) (2 * e->expires - e->created) >= nocopy_now(); }

/* Lock-free; between borrow_begin/end. Forced inline: gcc outlined it once fetch() got more callers (+9 ns copy fetch). The entry's segment is pinned before it is touched.
 * A miss counts only if no delete shifted slots meanwhile (mseq): one may move the key backwards past the probe.
 * stale: also return an entry that expired, if nocopy_stale_ok (constant at every call site: folds away). */
static inline __attribute__((always_inline)) nocopy_entry *nocopy_find_ex(nocopy_ctx *c, uint64_t h, const char *k, uint32_t klen, bool stale)
{
	uint32_t si = nocopy_shard_idx(h); /* not sh - shards: that divides by sizeof(nocopy_shard) */
	nocopy_shard *sh = &nocopy_a->shards[si];
	const nocopy_slot *t = nocopy_tabs[si];
	uint32_t m = nocopy_tab_mask, tag = nocopy_slot_tag(h);
	for (;;) {
	retry:;
		uint32_t seq = __atomic_load_n(&sh->mseq, __ATOMIC_ACQUIRE);
		for (uint32_t n = 0, i = tag & m; n <= m; n++, i = (i + 1) & m) {
			nocopy_slot s = __atomic_load_n(&t[i], __ATOMIC_ACQUIRE);
			if (!s) {
				break;
			}
			if ((uint32_t) (s >> 32) != tag) {
				continue;
			}
			nocopy_entry *e = nocopy_slot_entry(s);
			if (nocopy_hold(c, e) && __atomic_load_n(&t[i], __ATOMIC_ACQUIRE) != s) {
				goto retry; /* unlinked while we were pinning: the memory may be reused */
			}
			if (e->h == h && e->klen == klen && nocopy_key_eq(nocopy_key(e), k, klen)) {
				if (nocopy_expired(e)) {
					return stale && nocopy_stale_ok(e) ? e : NULL;
				}
				uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
				if (f < NOCOPY_HOT_MAX && (!f || !(nocopy_rand(c) & ((1u << f) - 1)))) {
					__atomic_store_n(&e->hot, f + 1, __ATOMIC_RELAXED); /* p = 1/2^f: ~log2(reads) writes, the line stays shared */
					__atomic_add_fetch(&nocopy_segs[nocopy_seg_of(e)].reads, 1 << f, __ATOMIC_RELAXED);
				}
				return e;
			}
		}
		/* no fence: the slot loads above are acquire loads, so this one cannot move before them */
		if (!(seq & 1) && __atomic_load_n(&sh->mseq, __ATOMIC_RELAXED) == seq) {
			return NULL;
		}
		nocopy_cpu_relax();
	}
}

static inline __attribute__((always_inline)) nocopy_entry *nocopy_find(nocopy_ctx *c, uint64_t h, const char *k, uint32_t klen)
{
	return nocopy_find_ex(c, h, k, klen, false);
}

#endif
