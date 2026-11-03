/* stash: zero-copy shared-memory store with an apcu_* compatibility layer */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"
#include "SAPI.h"
#include "ext/pcre/php_pcre.h"
#include "php_stash.h"
#include "stash_arginfo.h"
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#ifdef ZTS
# error "stash does not support thread-safe (ZTS) PHP builds yet"
#endif

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

#define STASH_MAX_WORKERS 1024 /* ponytail: more live processes than this cannot claim a slot */
#define STASH_MAX_SEGS 256
#define SEG_WORDS (STASH_MAX_SEGS / 64)
#define STASH_MAX_DEPTH 64
#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)

/* The arena is a log of segments: entries are appended, never freed one by one.
 * A segment goes back to the pool once nothing live is in it and no worker pins it. */
typedef struct stash_entry {
	zend_string *key;
	zval val;
	time_t expires;
	time_t created;
	uint64_t seq; /* insertion order, for iterators */
	size_t size;  /* whole block, header included */
	bool serialized; /* val is a serialized string; unserialize on fetch */
	bool live;    /* published and not yet unlinked */
} stash_entry;

/* low 32 bits: arena offset / 8, high 32: hash tag (so a probe rarely touches the entry); 0 empty, 1 tombstone */
typedef uint64_t slot_t;
#define SLOT_TOMB ((slot_t) 1)

enum { SEG_FREE, SEG_OPEN, SEG_SEALED, SEG_DEAD, SEG_CONT };

typedef struct {
	uint8_t state;
	uint32_t run;     /* head of a run: number of segments */
	uint32_t pending; /* allocated, not yet published */
	size_t used;      /* bytes appended */
	size_t live;      /* bytes of live entries */
	time_t max_exp;   /* latest expiry of its entries; 0 in the ttl=0 bucket (never) */
} stash_seg;

/* Entries go to an open segment per TTL bucket (Segcache), so a segment expires as a whole:
 * ttl 0, then < 8s, < 64s, < 512s, < 4096s, < 32768s, longer. */
#define TTL_BUCKETS 7

/* One cache line per worker: the segments it pins, and its counters. */
typedef struct {
	int pid;
	uint64_t bits[SEG_WORDS];
	uint64_t hits;
	uint64_t misses;
} __attribute__((aligned(64))) stash_worker;

typedef struct {
	int lock __attribute__((aligned(64))); /* alone on its cache line: spinners must not steal the holder's metadata */
	int32_t cur[TTL_BUCKETS] __attribute__((aligned(64))); /* segment being appended to per bucket, or -1 */
	size_t header_size;
	uint32_t nslots, nsegs, nfree, ndead;
	uint32_t nworkers; /* high-water mark of claimed worker slots: scans stop here */
	bool hint;     /* a segment just died: worth a reclaim pass */
	time_t start_time, exp_idle; /* exp_idle: second in which a full expiry pass found nothing */
	uint32_t exp_next;  /* expiry cursor: segment ... */
	size_t exp_off;     /* ... and offset of the next entry to look at */
	uint64_t inserts, expunges, live_bytes, evacuated;
	stash_worker workers[STASH_MAX_WORKERS];
	stash_seg segs[STASH_MAX_SEGS];
	slot_t slots[]; /* nslots entries follow */
} stash_arena;

static stash_arena *arena;
static size_t arena_size = 32 * 1024 * 1024;
static size_t seg_size;
static unsigned seg_shift;
static char *seg_base;
static zend_long ini_hint = 4096, ini_ttl;
static bool ini_enabled = true, ini_enable_cli, request_scope;
static bool apc_on;               /* apc.enabled, and apc.enable_cli under the CLI SAPI */
static int my_slot = -1;
static int scope_depth;           /* >0: this process may be handed zero-copy pointers */
static int pin_refs;              /* calls currently reading the arena */
static uint64_t mine[SEG_WORDS];  /* segments this process has published as pinned */
static uint64_t keep[SEG_WORDS];  /* subset kept past the call: a zero-copy pointer left the extension */
static bool transient;            /* mine has bits that are not in keep */
static uint32_t slot_mask;        /* arena->nslots - 1, kept local so readers never touch the header's cache lines */

#define MASK slot_mask

static inline uint32_t seg_index(const void *p) { return (uint32_t) (((const char *) p - seg_base) >> seg_shift); }
static inline char *seg_start(uint32_t i) { return seg_base + ((size_t) i << seg_shift); }
static inline bool slot_live(slot_t s) { return (uint32_t) s > 1; }
static inline stash_entry *slot_entry(slot_t s) { return (stash_entry *) ((char *) arena + ((size_t) (uint32_t) s << 3)); }
static inline uint32_t slot_tag(zend_ulong h) { return (uint32_t) ((h >> 32) ^ h); }
static inline slot_t slot_make(const stash_entry *e, zend_ulong h)
{
	return ((slot_t) slot_tag(h) << 32) | (uint32_t) (((const char *) e - (const char *) arena) >> 3);
}

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
}

/* Test-and-test-and-set; yields once the wait gets long (holder preempted or compacting).
 * ponytail: a writer dying inside the lock deadlocks everyone; robust pthread mutex (EOWNERDEAD) on Linux */
static void arena_lock(void)
{
	unsigned spins = 0;
	while (__atomic_exchange_n(&arena->lock, 1, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&arena->lock, __ATOMIC_RELAXED)) {
			if (++spins < 128) {
				cpu_relax();
			} else {
				sched_yield();
			}
		}
	}
}

static void arena_unlock(void)
{
	__atomic_store_n(&arena->lock, 0, __ATOMIC_RELEASE);
}

/* ---- workers and pins ---- */

static void bits_clear(int slot)
{
	for (int w = 0; w < SEG_WORDS; w++) {
		__atomic_store_n(&arena->workers[slot].bits[w], 0, __ATOMIC_SEQ_CST);
	}
}

static int claim_worker(void)
{
	int me = getpid();
	for (int i = 0; i < STASH_MAX_WORKERS; i++) {
		int expect = 0;
		if (__atomic_compare_exchange_n(&arena->workers[i].pid, &expect, me, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			uint32_t cur = __atomic_load_n(&arena->nworkers, __ATOMIC_RELAXED);
			while (cur < (uint32_t) i + 1
			 && !__atomic_compare_exchange_n(&arena->nworkers, &cur, i + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
			return i;
		}
	}
	for (int i = 0; i < STASH_MAX_WORKERS; i++) {
		int p = arena->workers[i].pid;
		if (kill(p, 0) == -1 && errno == ESRCH
		 && __atomic_compare_exchange_n(&arena->workers[i].pid, &p, me, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			bits_clear(i);
			return i;
		}
	}
	return -1;
}

/* Publish "I may touch this segment" before dereferencing anything in it.
 * True if the pin is new: the caller must then re-check that what it found is still linked. */
static bool seg_hold(const void *p)
{
	uint32_t s = seg_index(p), w = s >> 6;
	uint64_t b = (uint64_t) 1 << (s & 63);
	if (mine[w] & b) {
		return false;
	}
	mine[w] |= b;
	transient = true;
	__atomic_store_n(&arena->workers[my_slot].bits[w], mine[w], __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return true;
}

/* A zero-copy pointer into e is leaving the extension: keep its segment pinned. */
static void seg_keep(const void *p)
{
	uint32_t s = seg_index(p);
	keep[s >> 6] |= (uint64_t) 1 << (s & 63);
}

static void drop_transient(void)
{
	if (my_slot < 0 || !transient) {
		return;
	}
	for (int w = 0; w < SEG_WORDS; w++) {
		if (mine[w] & ~keep[w]) {
			mine[w] &= keep[w];
			__atomic_store_n(&arena->workers[my_slot].bits[w], mine[w], __ATOMIC_SEQ_CST);
		}
	}
	transient = false;
}

/* Drop the kept pins: the caller promises no earlier zero-copy result is used again. */
static void keep_release(void)
{
	memset(keep, 0, sizeof(keep));
	if (!pin_refs) {
		transient = true;
		drop_transient();
	}
}

static void borrow_begin(void)
{
	pin_refs++;
}

static void borrow_end(void)
{
	if (--pin_refs == 0) {
		drop_transient();
	}
}

/* A forked child gets its own worker slot and inherits the parent's pins. */
static void atfork_child(void)
{
	if (!arena) {
		return;
	}
	my_slot = claim_worker();
	if (my_slot >= 0) {
		for (int w = 0; w < SEG_WORDS; w++) {
			__atomic_store_n(&arena->workers[my_slot].bits[w], mine[w], __ATOMIC_SEQ_CST);
		}
	}
}

static bool tracked(void)
{
	if (my_slot < 0) {
		zend_throw_error(NULL, "stash: no free worker slot");
		return false;
	}
	return true;
}

/* ---- segments (all with the lock held) ---- */

static bool expired(const stash_entry *e)
{
	return e->expires && e->expires <= time(NULL);
}

static void seg_check(uint32_t s)
{
	stash_seg *g = &arena->segs[s];
	if (g->state == SEG_SEALED && !g->live && !g->pending) {
		g->state = SEG_DEAD;
		arena->ndead++;
		arena->hint = true;
	}
}

static void seg_free(uint32_t s)
{
	uint32_t k = arena->segs[s].run;
	if (s == arena->exp_next) {
		arena->exp_off = 0; /* the cursor's offset would land mid-entry once the segment is reused */
	}
#if ZEND_DEBUG
	memset(seg_start(s), 0x5A, (size_t) k << seg_shift); /* make use-after-free loud */
#endif
	for (uint32_t j = s; j < s + k; j++) {
		memset(&arena->segs[j], 0, sizeof(stash_seg)); /* SEG_FREE */
	}
	arena->nfree += k;
	arena->ndead--;
}

/* Hand back every dead segment that no live worker pins. */
static void reclaim(void)
{
	arena->hint = false;
	if (!arena->ndead) {
		return;
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	uint64_t held[SEG_WORDS] = {0};
	uint32_t nw = __atomic_load_n(&arena->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		uint64_t b[SEG_WORDS], any = 0;
		for (int w = 0; w < SEG_WORDS; w++) {
			any |= b[w] = __atomic_load_n(&arena->workers[i].bits[w], __ATOMIC_SEQ_CST);
		}
		if (!any) {
			continue;
		}
		int pid = arena->workers[i].pid;
		if (pid && kill(pid, 0) == -1 && errno == ESRCH) {
			bits_clear(i); /* died holding pins */
			continue;
		}
		for (int w = 0; w < SEG_WORDS; w++) {
			held[w] |= b[w];
		}
	}
	for (uint32_t s = 0; s < arena->nsegs; s++) {
		if (arena->segs[s].state == SEG_DEAD && !(held[s >> 6] >> (s & 63) & 1)) {
			seg_free(s);
		}
	}
}

/* An entry stops being reachable: its bytes are garbage from now on. */
static void entry_die(stash_entry *e)
{
	uint32_t s = seg_index(e);
	e->live = false;
	arena->segs[s].live -= e->size;
	arena->live_bytes -= e->size;
	seg_check(s);
}

static void entry_birth(stash_entry *e)
{
	uint32_t s = seg_index(e);
	e->live = true;
	arena->segs[s].pending--;
	arena->segs[s].live += e->size;
	arena->live_bytes += e->size;
}

static void entry_abort(stash_entry *e)
{
	uint32_t s = seg_index(e);
	arena->segs[s].pending--;
	seg_check(s);
}

static int ttl_bucket(time_t ttl)
{
	int b = ttl ? 1 : 0;
	while (b && b < TTL_BUCKETS - 1 && ttl >= ((time_t) 1 << (3 * b))) {
		b++;
	}
	return b;
}

static stash_entry *seg_carve(uint32_t s, size_t total, time_t exp)
{
	stash_seg *g = &arena->segs[s];
	stash_entry *e = (stash_entry *) (seg_start(s) + g->used);
	g->used += total;
	g->pending++;
	if (exp > g->max_exp) {
		g->max_exp = exp;
	}
	e->size = total;
	e->live = false;
	return e;
}

/* total: aligned block size. reserve: may take the last free segment (kept back for compaction).
 * exp: the entry's expiry (0 never); it picks the TTL bucket. */
static stash_entry *seg_alloc(size_t total, bool reserve, time_t exp, time_t now)
{
	int b = ttl_bucket(exp ? exp - now : 0);
	uint32_t spare = reserve ? 0 : 1;
	if (total > seg_size / 2) { /* a run of contiguous segments */
		uint32_t k = (uint32_t) ((total + seg_size - 1) >> seg_shift);
		if (arena->nfree < k + spare) {
			return NULL;
		}
		for (uint32_t s = 0, n = 0; s < arena->nsegs; s++) {
			n = arena->segs[s].state == SEG_FREE ? n + 1 : 0;
			if (n == k) {
				uint32_t head = s + 1 - k;
				for (uint32_t j = head; j <= s; j++) {
					arena->segs[j].state = SEG_CONT;
				}
				arena->segs[head].state = SEG_SEALED;
				arena->segs[head].run = k;
				arena->nfree -= k;
				return seg_carve(head, total, exp);
			}
		}
		return NULL;
	}
	int32_t cur = arena->cur[b];
	if (cur >= 0 && arena->segs[cur].used + total <= seg_size) {
		return seg_carve(cur, total, exp);
	}
	if (arena->nfree < 1 + spare) {
		return NULL;
	}
	if (cur >= 0) {
		arena->segs[cur].state = SEG_SEALED;
		arena->cur[b] = -1;
		seg_check(cur);
	}
	for (uint32_t s = 0; s < arena->nsegs; s++) {
		if (arena->segs[s].state == SEG_FREE) {
			arena->segs[s].state = SEG_OPEN;
			arena->segs[s].run = 1;
			arena->nfree--;
			arena->cur[b] = s;
			return seg_carve(s, total, exp);
		}
	}
	return NULL;
}

static void slot_clear(uint32_t i);
static bool calc_zval(zval *z, int depth, size_t *n, int *why);
static void persist_zval(zval *z);
static char *cursor;
static zend_string *persist_str(const zend_string *s);

/* Slot that links the live entry e, or UINT32_MAX. */
static uint32_t slot_of(const stash_entry *e)
{
	zend_ulong h = ZSTR_H(e->key);
	for (uint32_t i = h & MASK, k = 0; k <= MASK; k++, i = (i + 1) & MASK) {
		if (slot_live(arena->slots[i]) && slot_entry(arena->slots[i]) == e) {
			return i;
		}
	}
	return UINT32_MAX;
}

static void entry_unlink(stash_entry *e)
{
	uint32_t i = slot_of(e);
	if (i != UINT32_MAX) {
		slot_clear(i);
	}
	entry_die(e);
}

/* Unlinks entries of segments whose entries have all expired, looking at most `budget` entries per call
 * so no single write pays for a second's worth of expiry. No slot scan. True if anything was unlinked. */
static bool expire_step(time_t now, int budget)
{
	bool did = false;
	if (arena->exp_idle == now) {
		return false;
	}
	for (uint32_t seen = 0; seen < arena->nsegs; seen++) {
		uint32_t s = arena->exp_next;
		stash_seg *g = &arena->segs[s];
		if ((g->state == SEG_OPEN || g->state == SEG_SEALED) && g->live && g->max_exp && g->max_exp <= now) {
			while (arena->exp_off < g->used) {
				if (budget-- <= 0) {
					return did; /* resume here on the next write */
				}
				stash_entry *e = (stash_entry *) (seg_start(s) + arena->exp_off);
				arena->exp_off += e->size;
				if (e->live) {
					entry_unlink(e);
					did = true;
				}
			}
		}
		arena->exp_off = 0;
		arena->exp_next = (s + 1) % arena->nsegs;
	}
	arena->exp_idle = now;
	return did;
}

/* Moves the live entries of the sparsest sealed segment forward (expired ones are dropped); the segment dies. */
static bool evacuate(void)
{
	uint32_t victim = UINT32_MAX;
	for (uint32_t s = 0; s < arena->nsegs; s++) {
		stash_seg *g = &arena->segs[s];
		if (g->state == SEG_SEALED && g->run == 1 && !g->pending && g->live <= seg_size / 2
		 && (victim == UINT32_MAX || g->live < arena->segs[victim].live)) {
			victim = s;
		}
	}
	if (victim == UINT32_MAX) {
		return false;
	}
	char *p = seg_start(victim), *end = p + arena->segs[victim].used;
	while (p < end) {
		stash_entry *old = (stash_entry *) p;
		p += old->size;
		if (!old->live) {
			continue;
		}
		if (expired(old)) {
			entry_unlink(old);
			continue;
		}
		size_t body = 0, head = SALIGN(sizeof(stash_entry));
		calc_zval(&old->val, 0, &body, NULL);
		/* now = created: keeps the entry in its original TTL bucket */
		stash_entry *n = seg_alloc(head + SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(old->key))) + body, true, old->expires, old->created);
		if (!n) {
			return false;
		}
		arena->evacuated += n->size;
		size_t total = n->size;
		cursor = (char *) n + head;
		n->key = persist_str(old->key);
		ZVAL_COPY_VALUE(&n->val, &old->val);
		persist_zval(&n->val);
		ZEND_ASSERT(cursor == (char *) n + total);
		n->expires = old->expires;
		n->created = old->created;
		n->seq = old->seq;
		n->serialized = old->serialized;
		uint32_t i = slot_of(old);
		if (i != UINT32_MAX) {
			entry_birth(n);
			__atomic_store_n(&arena->slots[i], slot_make(n, ZSTR_H(n->key)), __ATOMIC_RELEASE);
		} else {
			entry_abort(n);
		}
		entry_die(old);
	}
	return true;
}

/* Lock held. Expire, reclaim and compact until `total` bytes fit, or give up. */
static stash_entry *alloc_entry(size_t total, time_t exp, time_t now)
{
	expire_step(now, 64);
	stash_entry *e = seg_alloc(total, false, exp, now);
	if (!e) {
		reclaim();
		e = seg_alloc(total, false, exp, now);
	}
	while (!e && expire_step(now, 4096)) { /* under pressure: catch up on expiry before copying live data */
		reclaim();
		e = seg_alloc(total, false, exp, now);
	}
	for (uint32_t tries = arena->nsegs; !e && tries && evacuate(); tries--) {
		reclaim();
		e = seg_alloc(total, false, exp, now);
	}
	return e;
}

/* ---- sizing and persisting ---- */

enum { WHY_NONE = 0, WHY_FALLBACK, WHY_RESOURCE };

static bool calc_zval(zval *z, int depth, size_t *n, int *why);

static bool calc_array(zend_array *a, int depth, size_t *n, int *why)
{
	if (zend_hash_num_elements(a) == 0) {
		return true;
	}
	*n += SALIGN(sizeof(zend_array)) + SALIGN(HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : HT_USED_SIZE(a));
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if (!calc_zval(v, depth + 1, n, why)) {
				return false;
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key) {
				*n += SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(b->key)));
			}
			if (!calc_zval(&b->val, depth + 1, n, why)) {
				return false;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return true;
}

/* Adds the out-of-line bytes persist_zval() will consume.
 * why != NULL: refused values set *why so the caller can fall back or warn; else they throw. */
static bool calc_zval(zval *z, int depth, size_t *n, int *why)
{
	if (depth > STASH_MAX_DEPTH) {
		if (why) {
			*why = WHY_FALLBACK;
		} else {
			zend_value_error("stash: value is nested too deeply");
		}
		return false;
	}
	switch (Z_TYPE_P(z)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE:
			return true;
		case IS_REFERENCE:
			if (why && Z_REFCOUNT_P(z) > 1) { /* a shared reference must stay shared: serialize */
				*why = WHY_FALLBACK;
				return false;
			}
			return calc_zval(Z_REFVAL_P(z), depth + 1, n, why);
		case IS_STRING:
			*n += SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN_P(z)));
			return true;
		case IS_ARRAY:
			return calc_array(Z_ARR_P(z), depth, n, why);
		default:
			if (why) {
				*why = Z_TYPE_P(z) == IS_RESOURCE ? WHY_RESOURCE : WHY_FALLBACK;
			} else {
				zend_argument_type_error(2, "must not contain objects or resources");
			}
			return false;
	}
}

static char *cursor; /* write position inside the block being filled */

static void *take(size_t n)
{
	void *p = cursor;
	cursor += SALIGN(n);
	return p;
}

static zend_string *persist_str(const zend_string *s)
{
	size_t size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(s));
	zend_string *p = take(size);
	memcpy(p, s, size);
	zend_string_hash_val(p);
	GC_SET_REFCOUNT(p, 2);
	/* not IS_STR_PERMANENT: that promises process lifetime (PCRE then keeps the pointer as a cache key) */
	GC_TYPE_INFO(p) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	return p;
}

static void persist_zval(zval *z);

static zend_array *persist_array(const zend_array *src)
{
	if (zend_hash_num_elements(src) == 0) {
		return (zend_array *) &zend_empty_array;
	}
	zend_array *ht = take(sizeof(zend_array));
	memcpy(ht, src, sizeof(zend_array));

	size_t size = HT_IS_PACKED(src) ? HT_PACKED_USED_SIZE(src) : HT_USED_SIZE(src);
	void *data = take(size);
	memcpy(data, HT_GET_DATA_ADDR(src), size);
	HT_SET_DATA_ADDR(ht, data);

	ht->nInternalPointer = 0;
	GC_SET_REFCOUNT(ht, 2);
	GC_TYPE_INFO(ht) = GC_ARRAY | IS_ARRAY_IMMUTABLE;

	if (HT_IS_PACKED(ht)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(ht, v) {
			persist_zval(v);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(ht, b) {
			if (b->key) {
				b->key = persist_str(b->key);
			}
			persist_zval(&b->val);
		} ZEND_HASH_FOREACH_END();
	}

	/* serialization does not keep the next free index, so neither do we: max int key + 1 */
	zend_long max = ZEND_LONG_MIN;
	zend_ulong ih;
	zend_string *ik;
	ZEND_HASH_FOREACH_KEY(ht, ih, ik) {
		if (!ik && (zend_long) ih > max) {
			max = (zend_long) ih;
		}
	} ZEND_HASH_FOREACH_END();
	ht->nNextFreeElement = (max == ZEND_LONG_MIN || max == ZEND_LONG_MAX) ? max : max + 1;
	return ht;
}

/* Rewrites *z in place so everything it points to lives in the current block. */
static void persist_zval(zval *z)
{
	switch (Z_TYPE_P(z)) {
		case IS_REFERENCE:
			ZVAL_COPY_VALUE(z, Z_REFVAL_P(z));
			persist_zval(z);
			break;
		case IS_STRING:
			Z_STR_P(z) = persist_str(Z_STR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		case IS_ARRAY:
			Z_ARR_P(z) = persist_array(Z_ARR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		default: /* scalars: calc_zval() already rejected everything else */
			break;
	}
}

/* ---- leaving shared memory: deep copies ---- */

static bool in_arena(const void *p)
{
	return (uintptr_t) p - (uintptr_t) arena < arena_size;
}

static void unshare(zval *z, int depth);
static void unshare_contents(zend_array *a, int depth);

/* Deep copy of an arena value into ordinary request memory. */
static void copy_out(zval *dst, const zval *src)
{
	switch (Z_TYPE_P(src)) {
		case IS_STRING:
			ZVAL_STRINGL(dst, Z_STRVAL_P(src), Z_STRLEN_P(src));
			break;
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(src);
			if (zend_hash_num_elements(a) == 0) {
				ZVAL_EMPTY_ARRAY(dst);
				break;
			}
			/* bulk-copy the buckets, then repoint whatever still lives in the arena */
			ZVAL_ARR(dst, zend_array_dup(a));
			unshare_contents(Z_ARR_P(dst), 0);
			break;
		}
		default:
			ZVAL_COPY_VALUE(dst, src);
	}
}

/* Repoints every key and value of a (private) array that still lives in the arena. */
static void unshare_contents(zend_array *a, int depth)
{
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			unshare(v, depth + 1);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key && in_arena(b->key)) {
				b->key = zend_string_init(ZSTR_VAL(b->key), ZSTR_LEN(b->key), 0);
				ZSTR_H(b->key) = b->h;
				HT_FLAGS(a) &= ~HASH_FLAG_STATIC_KEYS;
			}
			unshare(&b->val, depth + 1);
		} ZEND_HASH_FOREACH_END();
	}
}

/* Rewrites a user value in place so nothing in it (values or keys) points into the arena.
 * ponytail: does not look inside objects; values stored there escape the scope. */
static void unshare(zval *z, int depth)
{
	if (depth > STASH_MAX_DEPTH) {
		zend_value_error("stash: scope result is nested too deeply");
		return;
	}
	switch (Z_TYPE_P(z)) {
		case IS_REFERENCE:
			unshare(Z_REFVAL_P(z), depth + 1);
			break;
		case IS_STRING:
			if (in_arena(Z_STR_P(z))) {
				zval t;
				copy_out(&t, z);
				ZVAL_COPY_VALUE(z, &t);
			}
			break;
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(z);
			if (in_arena(a)) {
				zval t;
				copy_out(&t, z);
				ZVAL_COPY_VALUE(z, &t);
			} else if (!(GC_FLAGS(a) & IS_ARRAY_IMMUTABLE)) {
				unshare_contents(a, depth);
			}
			break;
		}
	}
}

/* ---- lookup ---- */

static bool key_eq(const zend_string *a, const zend_string *b)
{
	return ZSTR_H(a) == ZSTR_H(b) && zend_string_equal_content((zend_string *) a, (zend_string *) b);
}

/* Tombstones are only needed while a probe chain continues behind them. */
static void slot_clear(uint32_t i)
{
	if (arena->slots[(i + 1) & MASK] != 0) {
		__atomic_store_n(&arena->slots[i], SLOT_TOMB, __ATOMIC_RELEASE);
		return;
	}
	__atomic_store_n(&arena->slots[i], 0, __ATOMIC_RELEASE);
	for (uint32_t j = (i - 1) & MASK; arena->slots[j] == SLOT_TOMB; j = (j - 1) & MASK) {
		__atomic_store_n(&arena->slots[j], 0, __ATOMIC_RELEASE);
	}
}

/* Lock-free; call between borrow_begin()/borrow_end(). Pins the entry's segment before touching it. */
static stash_entry *find(zend_string *key)
{
	zend_ulong h = zend_string_hash_val(key);
	uint32_t tag = slot_tag(h);
retry:
	for (uint32_t n = 0, i = h & MASK; n <= MASK; n++, i = (i + 1) & MASK) {
		slot_t s = __atomic_load_n(&arena->slots[i], __ATOMIC_ACQUIRE);
		if (!s) {
			return NULL;
		}
		if (!slot_live(s) || (uint32_t) (s >> 32) != tag) {
			continue;
		}
		stash_entry *e = slot_entry(s);
		if (seg_hold(e) && __atomic_load_n(&arena->slots[i], __ATOMIC_ACQUIRE) != s) {
			goto retry; /* unlinked while we were pinning: the memory may be reused */
		}
		if (key_eq(e->key, key)) {
			return expired(e) ? NULL : e;
		}
	}
	return NULL;
}

/* Lock-free scan helper: the pinned entry in slot i, or NULL. Call between borrow_begin()/borrow_end(). */
static stash_entry *slot_pin(uint32_t i)
{
	for (;;) {
		slot_t s = __atomic_load_n(&arena->slots[i], __ATOMIC_ACQUIRE);
		if (!slot_live(s)) {
			return NULL;
		}
		stash_entry *e = slot_entry(s);
		if (!seg_hold(e) || __atomic_load_n(&arena->slots[i], __ATOMIC_ACQUIRE) == s) {
			return e;
		}
	}
}

/* Lock held. Slot for key: its own if present, else first tombstone/empty, else -1. */
static int locate(zend_string *key, stash_entry **found)
{
	zend_ulong h = zend_string_hash_val(key);
	uint32_t tag = slot_tag(h);
	int idx = -1;
	*found = NULL;
	for (uint32_t n = 0, i = h & MASK; n < arena->nslots; n++, i = (i + 1) & MASK) {
		slot_t s = arena->slots[i];
		if (!s) {
			return idx < 0 ? (int) i : idx;
		}
		if (!slot_live(s)) {
			if (idx < 0) {
				idx = i;
			}
		} else if ((uint32_t) (s >> 32) == tag && key_eq(slot_entry(s)->key, key)) {
			*found = slot_entry(s);
			return i;
		}
	}
	return idx;
}

/* ---- core operations ---- */

typedef enum { PUT_OK, PUT_EXISTS, PUT_FULL } put_result;

#define STASH_INLINE_PUT 2048 /* blocks up to this size are filled under the lock: one lock round trip per write */

/* Build an invisible entry in a fresh block. NULL: arena full, refused value, or exception pending.
 * *locked: the lock is still held (small block); pass it on to commit(). */
static stash_entry *build_entry(zend_string *key, zval *val, zend_long ttl, bool apcu_semantics, bool *locked)
{
	*locked = false;
	zval ser;
	bool serialized = false;
	size_t head = SALIGN(sizeof(stash_entry));
	size_t payload = head + SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(key)));
	size_t body = 0;
	int why = WHY_NONE;

	if (!ttl) {
		ttl = ini_ttl;
	}
	if (!calc_zval(val, 0, &body, apcu_semantics ? &why : NULL)) {
		if (!why) {
			return NULL; /* strict mode already threw */
		}
		if (why == WHY_RESOURCE) {
			php_error_docref(NULL, E_WARNING, "Cannot store resources in apcu cache");
			return NULL;
		}
		smart_str buf = {0};
		BG(serialize_lock)++;
		php_serialize_data_t h = php_var_serialize_init();
		php_var_serialize(&buf, val, &h);
		php_var_serialize_destroy(h);
		BG(serialize_lock)--;
		if (EG(exception)) {
			smart_str_free(&buf);
			return NULL;
		}
		smart_str_0(&buf);
		ZVAL_STR(&ser, buf.s);
		val = &ser;
		serialized = true;
		body = 0;
		calc_zval(val, 0, &body, NULL);
	}
	payload += body;

	arena_lock();
	time_t now = time(NULL), exp = ttl ? now + ttl : 0; /* inside the lock: measured faster under write contention than before it */
	stash_entry *e = alloc_entry(payload, exp, now);
	*locked = e && payload <= STASH_INLINE_PUT;
	if (!*locked) {
		arena_unlock();
	}

	if (e) {
		cursor = (char *) e + head;
		e->key = persist_str(key);
		ZVAL_COPY_VALUE(&e->val, val);
		persist_zval(&e->val);
		ZEND_ASSERT(cursor == (char *) e + payload);
		e->expires = exp;
		e->created = now;
		e->serialized = serialized;
	}
	if (serialized) {
		zend_string_release(Z_STR(ser));
	}
	return e;
}

/* Publish e under key; add_only fails if a live entry exists. Consumes e either way. Unlocks. */
static put_result commit(stash_entry *e, zend_string *key, bool add_only, bool locked)
{
	put_result r = PUT_OK;
	if (!locked) {
		arena_lock();
	}
	stash_entry *old;
	int idx = locate(key, &old);
	if (idx < 0) {
		entry_abort(e);
		r = PUT_FULL;
	} else if (add_only && old && !expired(old)) {
		entry_abort(e);
		r = PUT_EXISTS;
	} else {
		e->seq = ++arena->inserts;
		entry_birth(e);
		__atomic_store_n(&arena->slots[idx], slot_make(e, zend_string_hash_val(key)), __ATOMIC_RELEASE);
		if (old) {
			entry_die(old);
		}
		if (arena->hint) {
			reclaim();
		}
	}
	arena_unlock();
	return r;
}

static bool do_store(zend_string *key, zval *val, zend_long ttl, bool add_only, bool apcu_semantics)
{
	bool locked;
	stash_entry *e = build_entry(key, val, ttl, apcu_semantics, &locked);
	return e && commit(e, key, add_only, locked) == PUT_OK;
}

static bool do_delete(zend_string *key)
{
	arena_lock();
	stash_entry *old;
	int idx = locate(key, &old);
	bool found = old && !expired(old);
	if (old) {
		slot_clear(idx);
		entry_die(old);
		if (arena->hint) {
			reclaim();
		}
	}
	arena_unlock();
	return found;
}

/* Unserialize with a fresh state: php_unserialize_with_options() would share the outer
 * unserialize()'s reference table when called re-entrantly (e.g. from Serializable::unserialize). */
static bool cache_unserialize(zval *rv, const char *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *) buf;
	BG(serialize_lock)++; /* forces a private state instead of the outer call's */
	php_unserialize_data_t h = php_var_unserialize_init();
	bool ok = php_var_unserialize(rv, &p, p + len, &h);
	php_var_unserialize_destroy(h);
	BG(serialize_lock)--;
	if (!ok || EG(exception)) {
		zval_ptr_dtor(rv);
		ZVAL_NULL(rv);
		return false;
	}
	if (Z_ISREF_P(rv)) { /* a top-level cycle comes back as a reference; functions must not return one */
		zend_unwrap_reference(rv);
	}
	return true;
}

/* Fetch into rv. Outside a scope the result is a private copy; inside, zero-copy. */
static bool fetch_value(zend_string *key, zval *rv)
{
	bool copy = scope_depth == 0;
	borrow_begin();
	stash_entry *e = find(key);
	bool hit = e != NULL;
	ZVAL_NULL(rv);
	if (e) {
		if (e->serialized) {
			hit = cache_unserialize(rv, Z_STRVAL(e->val), Z_STRLEN(e->val));
		} else if (copy) {
			copy_out(rv, &e->val);
		} else {
			ZVAL_COPY_VALUE(rv, &e->val);
			seg_keep(e); /* a pointer into the arena escaped: stay pinned until the scope ends */
		}
	}
	uint64_t *ctr = hit ? &arena->workers[my_slot].hits : &arena->workers[my_slot].misses;
	__atomic_store_n(ctr, *ctr + 1, __ATOMIC_RELAXED); /* single writer per slot: no RMW needed */
	borrow_end();
	return hit;
}

/* ---- snapshots and the iterator ---- */

#define APC_LIST_ACTIVE   0x1
#define APC_LIST_DELETED  0x2
#define APC_ITER_TYPE      (1 << 0)
#define APC_ITER_KEY       (1 << 1)
#define APC_ITER_VALUE     (1 << 2)
#define APC_ITER_NUM_HITS  (1 << 3)
#define APC_ITER_MTIME     (1 << 4)
#define APC_ITER_CTIME     (1 << 5)
#define APC_ITER_DTIME     (1 << 6)
#define APC_ITER_ATIME     (1 << 7)
#define APC_ITER_REFCOUNT  (1 << 8)
#define APC_ITER_MEM_SIZE  (1 << 9)
#define APC_ITER_TTL       (1 << 10)
#define APC_ITER_ALL       0xffffffffL

typedef struct {
	zend_string *key;
	time_t created, expires;
	size_t mem;
	uint64_t seq;
} snap_row;

typedef struct {
	snap_row *rows;
	uint32_t count, pos;
	pcre_cache_entry *pce;
	HashTable *keys; /* explicit key set, or NULL */
	zend_long format, list;
	bool initialized, built, valid;
	zend_string *cur_key;
	zval cur_item;
	zend_object std;
} stash_iter;

static zend_class_entry *iter_ce;
static zend_object_handlers iter_handlers;

#define ITER(o) ((stash_iter *) ((char *) (o) - offsetof(stash_iter, std)))

static void rows_free(stash_iter *it)
{
	for (uint32_t i = 0; i < it->count; i++) {
		zend_string_release(it->rows[i].key);
	}
	efree(it->rows);
	it->rows = NULL;
	it->count = 0;
}

static int row_cmp(const void *a, const void *b)
{
	uint64_t x = ((const snap_row *) a)->seq, y = ((const snap_row *) b)->seq;
	return x < y ? -1 : x > y;
}

static bool row_matches(stash_iter *it, zend_string *key)
{
	if (it->pce) {
		zval rv;
		ZVAL_UNDEF(&rv);
		php_pcre_match_impl(it->pce, key, &rv, NULL, 0, 0, 0);
		bool m = Z_TYPE(rv) == IS_LONG && Z_LVAL(rv) > 0;
		zval_ptr_dtor(&rv);
		return m;
	}
	return !it->keys || zend_symtable_exists(it->keys, key);
}

/* Fresh insertion-ordered snapshot of the live keys that match the iterator's filter. */
static void iter_snapshot(stash_iter *it)
{
	if (it->rows) {
		rows_free(it);
	}
	it->pos = 0;
	if (it->list & APC_LIST_DELETED && !(it->list & APC_LIST_ACTIVE)) {
		return;
	}
	/* no lock: emalloc may bail out (OOM) and must never do so with the arena locked */
	uint32_t c = 0, cap = 64;
	snap_row *rows = safe_emalloc(cap, sizeof(snap_row), 0);
	borrow_begin();
	for (uint32_t i = 0; i < arena->nslots; i++) {
		stash_entry *e = slot_pin(i);
		if (!e || expired(e)) {
			continue;
		}
		if (c == cap) {
			cap *= 2;
			rows = safe_erealloc(rows, cap, sizeof(snap_row), 0);
		}
		rows[c].key = zend_string_init(ZSTR_VAL(e->key), ZSTR_LEN(e->key), 0);
		rows[c].created = e->created;
		rows[c].expires = e->expires;
		rows[c].mem = e->size;
		rows[c].seq = e->seq;
		c++;
	}
	borrow_end();

	qsort(rows, c, sizeof(snap_row), row_cmp);
	uint32_t keep = 0;
	for (uint32_t i = 0; i < c; i++) {
		if (row_matches(it, rows[i].key)) {
			rows[keep++] = rows[i];
		} else {
			zend_string_release(rows[i].key);
		}
	}
	it->rows = rows;
	it->count = keep;
}

/* Load the item at/after pos; entries deleted since the snapshot are skipped when a value is wanted. */
static void iter_load(stash_iter *it)
{
	zval_ptr_dtor(&it->cur_item);
	ZVAL_UNDEF(&it->cur_item);
	if (it->cur_key) {
		zend_string_release(it->cur_key);
		it->cur_key = NULL;
	}
	it->valid = false;

	while (it->pos < it->count) {
		snap_row *r = &it->rows[it->pos];
		zval value;
		ZVAL_UNDEF(&value);
		if ((it->format & APC_ITER_VALUE) && !fetch_value(r->key, &value)) {
			it->pos++;
			continue;
		}
		zval item;
		array_init(&item);
		zend_long f = it->format;
		if (f & APC_ITER_TYPE) add_assoc_string(&item, "type", "user");
		if (f & APC_ITER_KEY) add_assoc_str(&item, "key", zend_string_copy(r->key));
		if (f & APC_ITER_VALUE) add_assoc_zval(&item, "value", &value);
		if (f & APC_ITER_NUM_HITS) add_assoc_long(&item, "num_hits", 0);
		if (f & APC_ITER_MTIME) add_assoc_long(&item, "mtime", r->created);
		if (f & APC_ITER_CTIME) add_assoc_long(&item, "creation_time", r->created);
		if (f & APC_ITER_DTIME) add_assoc_long(&item, "deletion_time", 0);
		if (f & APC_ITER_ATIME) add_assoc_long(&item, "access_time", r->created);
		if (f & APC_ITER_REFCOUNT) add_assoc_long(&item, "ref_count", 0);
		if (f & APC_ITER_MEM_SIZE) add_assoc_long(&item, "mem_size", r->mem);
		if (f & APC_ITER_TTL) add_assoc_long(&item, "ttl", r->expires ? r->expires - r->created : 0);
		ZVAL_COPY_VALUE(&it->cur_item, &item);
		it->cur_key = zend_string_copy(r->key);
		it->valid = true;
		return;
	}
}

static zend_object *iter_create(zend_class_entry *ce)
{
	stash_iter *it = zend_object_alloc(sizeof(stash_iter), ce);
	zend_object_std_init(&it->std, ce);
	object_properties_init(&it->std, ce);
	ZVAL_UNDEF(&it->cur_item);
	it->std.handlers = &iter_handlers;
	return &it->std;
}

static void iter_free(zend_object *o)
{
	stash_iter *it = ITER(o);
	if (it->rows) {
		rows_free(it);
	}
	if (it->keys) {
		zend_array_destroy(it->keys);
	}
	if (it->cur_key) {
		zend_string_release(it->cur_key);
	}
	zval_ptr_dtor(&it->cur_item);
	zend_object_std_dtor(o);
}

/* The iterator for $this, snapshotted on first use. NULL (with an Error thrown) if uninitialized. */
static stash_iter *iter_this(zval *self)
{
	stash_iter *it = ITER(Z_OBJ_P(self));
	if (!it->initialized) {
		zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
		return NULL;
	}
	if (!it->built) {
		iter_snapshot(it);
		iter_load(it);
		it->built = true;
	}
	return it;
}

ZEND_METHOD(APCUIterator, __construct)
{
	zval *search = NULL;
	zend_long format = APC_ITER_ALL, chunk = 0, list = APC_LIST_ACTIVE;

	ZEND_PARSE_PARAMETERS_START(0, 4)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(search)
		Z_PARAM_LONG(format)
		Z_PARAM_LONG(chunk)
		Z_PARAM_LONG(list)
	ZEND_PARSE_PARAMETERS_END();
	(void) chunk;

	if (!apc_on) {
		zend_throw_error(NULL, "APC must be enabled to use APCUIterator");
		RETURN_THROWS();
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	stash_iter *it = ITER(Z_OBJ_P(ZEND_THIS));
	if (search && Z_TYPE_P(search) == IS_STRING) {
		if (Z_STRLEN_P(search)) {
			it->pce = pcre_get_compiled_regex_cache(Z_STR_P(search));
			if (!it->pce) {
				RETURN_THROWS(); /* PCRE already warned; iterator stays uninitialized */
			}
		}
	} else if (search && Z_TYPE_P(search) == IS_ARRAY) {
		zval yes, *v;
		ZVAL_TRUE(&yes);
		it->keys = zend_new_array(zend_hash_num_elements(Z_ARRVAL_P(search)));
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(search), v) {
			ZVAL_DEREF(v);
			if (Z_TYPE_P(v) == IS_STRING) {
				zend_symtable_update(it->keys, Z_STR_P(v), &yes);
			}
		} ZEND_HASH_FOREACH_END();
	} else if (search && Z_TYPE_P(search) != IS_NULL) {
		zend_argument_type_error(1, "must be of type array|string|null, %s given", zend_zval_type_name(search));
		RETURN_THROWS();
	}
	it->format = format;
	it->list = list;
	it->initialized = true;
}

ZEND_METHOD(APCUIterator, rewind)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = ITER(Z_OBJ_P(ZEND_THIS));
	if (!it->initialized) {
		zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
		RETURN_THROWS();
	}
	iter_snapshot(it);
	iter_load(it);
	it->built = true;
}

ZEND_METHOD(APCUIterator, next)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	if (it->valid) {
		it->pos++;
		iter_load(it);
	}
}

ZEND_METHOD(APCUIterator, valid)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_BOOL(it->valid);
}

ZEND_METHOD(APCUIterator, key)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	if (!it->valid) {
		zend_throw_error(NULL, "Cannot call key() on invalid iterator");
		RETURN_THROWS();
	}
	RETURN_STR_COPY(it->cur_key);
}

ZEND_METHOD(APCUIterator, current)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	if (!it->valid) {
		zend_throw_error(NULL, "Cannot call current() on invalid iterator");
		RETURN_THROWS();
	}
	RETURN_COPY(&it->cur_item);
}

/* ponytail: per-entry hit counts are not tracked (would put a shared write on every read) */
ZEND_METHOD(APCUIterator, getTotalHits)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_LONG(0);
}

ZEND_METHOD(APCUIterator, getTotalSize)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	zend_long total = 0;
	for (uint32_t i = 0; i < it->count; i++) {
		total += it->rows[i].mem;
	}
	RETURN_LONG(total);
}

ZEND_METHOD(APCUIterator, getTotalCount)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_LONG(it->count);
}

/* ---- stash_* ---- */

PHP_FUNCTION(stash_put)
{
	zend_string *key;
	zval *val;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(val)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	bool ok = do_store(key, val, ttl, false, false);
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(ok);
}

PHP_FUNCTION(stash_get)
{
	zend_string *key;
	zval *hit = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(hit)
	ZEND_PARSE_PARAMETERS_END();

	if (!tracked()) {
		RETURN_THROWS();
	}
	bool found = fetch_value(key, return_value);
	if (hit) {
		ZEND_TRY_ASSIGN_REF_BOOL(hit, found);
	}
}

PHP_FUNCTION(stash_delete)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(do_delete(key));
}

/* For long-running workers: promise that no earlier stash_get() result is used again. */
PHP_FUNCTION(stash_checkpoint)
{
	ZEND_PARSE_PARAMETERS_NONE();
	keep_release();
}

/* Zero-copy reads inside $callback; the result is made safe to keep afterwards. */
PHP_FUNCTION(stash_scope)
{
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_FUNC(fci, fcc)
	ZEND_PARSE_PARAMETERS_END();

	if (!tracked()) {
		RETURN_THROWS();
	}
	scope_depth++;
	fci.retval = return_value;
	zend_call_function(&fci, &fcc);
	if (scope_depth == 1 && !EG(exception)) {
		unshare(return_value, 0); /* must run before the pin is dropped: it reads the arena */
	}
	if (--scope_depth == 0) {
		keep_release();
	}
}

PHP_FUNCTION(stash_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();
	zend_long entries = 0, live, nfree, ndead, nsegs, evacuated;

	arena_lock();
	for (uint32_t i = 0; i < arena->nslots; i++) {
		entries += slot_live(arena->slots[i]);
	}
	live = arena->live_bytes;
	nfree = arena->nfree;
	ndead = arena->ndead;
	evacuated = arena->evacuated;
	nsegs = arena->nsegs;
	arena_unlock();

	uint64_t held[SEG_WORDS] = {0}, hits = 0, misses = 0;
	for (uint32_t i = 0; i < arena->nworkers; i++) {
		for (int w = 0; w < SEG_WORDS; w++) {
			held[w] |= __atomic_load_n(&arena->workers[i].bits[w], __ATOMIC_SEQ_CST);
		}
		hits += arena->workers[i].hits;
		misses += arena->workers[i].misses;
	}
	zend_long pinned = 0;
	for (int w = 0; w < SEG_WORDS; w++) {
		pinned += __builtin_popcountll(held[w]);
	}

	array_init(return_value);
	add_assoc_long(return_value, "size", arena_size);
	add_assoc_long(return_value, "used", live);
	add_assoc_long(return_value, "segment_size", seg_size);
	add_assoc_long(return_value, "segments", nsegs);
	add_assoc_long(return_value, "free_segments", nfree);
	add_assoc_long(return_value, "dead_segments", ndead);
	add_assoc_long(return_value, "pinned_segments", pinned);
	add_assoc_long(return_value, "entries", entries);
	add_assoc_long(return_value, "hits", hits);
	add_assoc_long(return_value, "misses", misses);
	add_assoc_long(return_value, "evacuated_bytes", evacuated);
}

/* ---- apcu_* ---- */

#define APC_WARN_KEYS "apc_delete() expects a string, array of strings, or APCIterator instance"

/* String form of a scalar key argument; caller releases. */
static zend_string *key_str(zval *z)
{
	ZVAL_DEREF(z);
	return zval_get_string(z);
}

static void apcu_store_impl(INTERNAL_FUNCTION_PARAMETERS, bool add_only)
{
	zval *key, *val = NULL;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ZVAL(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(val)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!val) {
			zend_wrong_parameters_count_error(2, 3);
			RETURN_THROWS();
		}
		if (!apc_on) {
			RETURN_FALSE;
		}
		if (!tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool ok = do_store(ks, val, ttl, add_only, true);
		zend_string_release(ks);
		if (EG(exception)) {
			RETURN_THROWS();
		}
		RETURN_BOOL(ok);
	}

	/* array form: returns the keys that failed, each mapped to -1 */
	if (apc_on && !tracked()) {
		RETURN_THROWS();
	}
	array_init(return_value);
	zend_ulong h;
	zend_string *k;
	zval *v;
	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(key), h, k, v) {
		zend_string *ks = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		bool ok = apc_on && do_store(ks, v, ttl, add_only, true);
		if (EG(exception)) {
			zend_string_release(ks);
			break;
		}
		if (!ok) {
			add_assoc_long_ex(return_value, ZSTR_VAL(ks), ZSTR_LEN(ks), -1);
		}
		zend_string_release(ks);
	} ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(apcu_store) { apcu_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, false); }
PHP_FUNCTION(apcu_add)   { apcu_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, true); }

PHP_FUNCTION(apcu_enabled)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(apc_on);
}

PHP_FUNCTION(apcu_fetch)
{
	zval *key, *success = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ZVAL(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(success)
	ZEND_PARSE_PARAMETERS_END();

	if (success) {
		ZEND_TRY_ASSIGN_REF_FALSE(success);
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!apc_on) {
			RETURN_FALSE;
		}
		if (!tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool hit = fetch_value(ks, return_value);
		zend_string_release(ks);
		if (!hit) {
			RETVAL_FALSE;
		} else if (success) {
			ZEND_TRY_ASSIGN_REF_TRUE(success);
		}
		return;
	}

	array_init(return_value);
	if (!apc_on) {
		return;
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	zval *v;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(key), v) {
		ZVAL_DEREF(v);
		zval t;
		if (Z_TYPE_P(v) == IS_STRING && fetch_value(Z_STR_P(v), &t)) {
			zend_symtable_update(Z_ARRVAL_P(return_value), Z_STR_P(v), &t);
		}
	} ZEND_HASH_FOREACH_END();
	if (success) {
		ZEND_TRY_ASSIGN_REF_TRUE(success);
	}
}

static bool exists(zend_string *key)
{
	borrow_begin();
	bool r = find(key) != NULL;
	borrow_end();
	return r;
}

PHP_FUNCTION(apcu_exists)
{
	zval *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(key)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!apc_on) {
			RETURN_FALSE;
		}
		if (!tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool r = exists(ks);
		zend_string_release(ks);
		RETURN_BOOL(r);
	}
	array_init(return_value);
	if (!apc_on) {
		return;
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	zval *v, yes;
	ZVAL_TRUE(&yes);
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(key), v) {
		ZVAL_DEREF(v);
		if (Z_TYPE_P(v) == IS_STRING && exists(Z_STR_P(v))) {
			zend_symtable_update(Z_ARRVAL_P(return_value), Z_STR_P(v), &yes);
		}
	} ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(apcu_delete)
{
	zval *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(key)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(key) == IS_OBJECT && instanceof_function(Z_OBJCE_P(key), iter_ce)) {
		stash_iter *it = ITER(Z_OBJ_P(key));
		if (!it->initialized) {
			zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
			RETURN_THROWS();
		}
		iter_snapshot(it);
		for (uint32_t i = 0; i < it->count; i++) {
			do_delete(it->rows[i].key);
		}
		rows_free(it);
		it->built = false;
		RETURN_TRUE;
	}
	if (Z_TYPE_P(key) == IS_STRING) {
		RETURN_BOOL(apc_on && do_delete(Z_STR_P(key)));
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		php_error_docref(NULL, E_WARNING, APC_WARN_KEYS);
		RETURN_FALSE;
	}
	array_init(return_value); /* keys that could not be deleted */
	zval *v;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(key), v) {
		ZVAL_DEREF(v);
		if (Z_TYPE_P(v) != IS_STRING) {
			php_error_docref(NULL, E_WARNING, APC_WARN_KEYS);
		}
		if (Z_TYPE_P(v) != IS_STRING || !apc_on || !do_delete(Z_STR_P(v))) {
			Z_TRY_ADDREF_P(v);
			add_next_index_zval(return_value, v);
		}
	} ZEND_HASH_FOREACH_END();
}

static void incdec(INTERNAL_FUNCTION_PARAMETERS, int sign)
{
	zend_string *key;
	zend_long step = 1, ttl = 0;
	zval *success = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 4)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(step)
		Z_PARAM_ZVAL(success)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (success) {
		ZEND_TRY_ASSIGN_REF_FALSE(success);
	}
	if (!apc_on) {
		RETURN_FALSE;
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	for (;;) {
		arena_lock();
		stash_entry *e;
		locate(key, &e);
		if (e && !expired(e)) {
			if (Z_TYPE(e->val) != IS_LONG || e->serialized) {
				arena_unlock();
				RETURN_FALSE;
			}
			/* in place and atomic: lock-free readers copy this word */
			zend_long nv = __atomic_add_fetch(&e->val.value.lval, sign * step, __ATOMIC_RELAXED);
			arena_unlock();
			if (success) {
				ZEND_TRY_ASSIGN_REF_TRUE(success);
			}
			RETURN_LONG(nv);
		}
		arena_unlock();

		zval init;
		ZVAL_LONG(&init, sign * step);
		bool locked;
		stash_entry *n = build_entry(key, &init, ttl, false, &locked);
		if (!n) {
			RETURN_FALSE;
		}
		put_result r = commit(n, key, true, locked);
		if (r == PUT_OK) {
			if (success) {
				ZEND_TRY_ASSIGN_REF_TRUE(success);
			}
			RETURN_LONG(sign * step);
		}
		if (r == PUT_FULL) {
			RETURN_FALSE;
		}
		/* PUT_EXISTS: someone created it first, go around and update theirs */
	}
}

PHP_FUNCTION(apcu_inc) { incdec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }
PHP_FUNCTION(apcu_dec) { incdec(INTERNAL_FUNCTION_PARAM_PASSTHRU, -1); }

PHP_FUNCTION(apcu_cas)
{
	zend_string *key;
	zend_long old, nw;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_LONG(old)
		Z_PARAM_LONG(nw)
	ZEND_PARSE_PARAMETERS_END();

	if (!apc_on) {
		RETURN_FALSE;
	}
	arena_lock();
	stash_entry *e;
	locate(key, &e);
	bool ok = e && !expired(e) && !e->serialized && Z_TYPE(e->val) == IS_LONG && Z_LVAL(e->val) == old;
	if (ok) {
		__atomic_store_n(&e->val.value.lval, nw, __ATOMIC_RELAXED);
	}
	arena_unlock();
	RETURN_BOOL(ok);
}

PHP_FUNCTION(apcu_clear_cache)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (!apc_on) {
		RETURN_TRUE;
	}
	arena_lock();
	for (uint32_t i = 0; i < arena->nslots; i++) {
		slot_t sl = arena->slots[i];
		if (slot_live(sl)) {
			__atomic_store_n(&arena->slots[i], SLOT_TOMB, __ATOMIC_RELEASE);
			entry_die(slot_entry(sl));
		}
	}
	arena->expunges++;
	reclaim();
	arena_unlock();
	RETURN_TRUE;
}

/* ponytail: not single-flight, two workers may both run $callback; milestone 3 */
PHP_FUNCTION(apcu_entry)
{
	zend_string *key;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(key)
		Z_PARAM_FUNC(fci, fcc)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (!apc_on) {
		RETURN_NULL();
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	if (fetch_value(key, return_value)) {
		return;
	}
	zval arg;
	ZVAL_STR(&arg, key);
	fci.param_count = 1;
	fci.params = &arg;
	fci.retval = return_value;
	if (zend_call_function(&fci, &fcc) == FAILURE || EG(exception)) {
		RETURN_THROWS();
	}
	do_store(key, return_value, ttl, true, true);
	if (EG(exception)) {
		RETURN_THROWS();
	}
}

PHP_FUNCTION(apcu_key_info)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	if (!apc_on) {
		RETURN_NULL();
	}
	if (!tracked()) {
		RETURN_THROWS();
	}
	borrow_begin();
	stash_entry *e = find(key);
	if (!e) {
		borrow_end();
		RETURN_NULL();
	}
	array_init(return_value);
	add_assoc_long(return_value, "hits", 0);
	add_assoc_long(return_value, "access_time", e->created);
	add_assoc_long(return_value, "mtime", e->created);
	add_assoc_long(return_value, "creation_time", e->created);
	add_assoc_long(return_value, "deletion_time", 0);
	add_assoc_long(return_value, "ttl", e->expires ? e->expires - e->created : 0);
	add_assoc_long(return_value, "refs", 0);
	borrow_end();
}

PHP_FUNCTION(apcu_cache_info)
{
	bool limited = false;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(limited)
	ZEND_PARSE_PARAMETERS_END();

	if (!apc_on) {
		php_error_docref(NULL, E_WARNING, "No APC info available.  Perhaps APC is not enabled? Check apc.enabled in your ini file");
		RETURN_FALSE;
	}

	zval list;
	array_init(&list);
	zend_long entries = 0;
	uint64_t hits = 0, misses = 0;

	borrow_begin(); /* pins, not the lock: building the list allocates */
	for (uint32_t i = 0; i < arena->nslots; i++) {
		stash_entry *e = slot_pin(i);
		if (!e || expired(e)) {
			continue;
		}
		entries++;
		if (!limited) {
			zval row;
			array_init(&row);
			add_assoc_str(&row, "info", zend_string_init(ZSTR_VAL(e->key), ZSTR_LEN(e->key), 0));
			add_assoc_long(&row, "ttl", e->expires ? e->expires - e->created : 0);
			add_assoc_long(&row, "num_hits", 0);
			add_assoc_long(&row, "mtime", e->created);
			add_assoc_long(&row, "creation_time", e->created);
			add_assoc_long(&row, "deletion_time", 0);
			add_assoc_long(&row, "access_time", e->created);
			add_assoc_long(&row, "ref_count", 0);
			add_assoc_long(&row, "mem_size", e->size);
			add_next_index_zval(&list, &row);
		}
	}
	borrow_end();
	zend_long mem = arena->live_bytes;
	uint64_t inserts = arena->inserts, expunges = arena->expunges;
	time_t start = arena->start_time;
	for (uint32_t i = 0; i < arena->nworkers; i++) {
		hits += arena->workers[i].hits;
		misses += arena->workers[i].misses;
	}

	array_init(return_value);
	add_assoc_long(return_value, "num_slots", arena->nslots);
	add_assoc_long(return_value, "ttl", ini_ttl);
	add_assoc_long(return_value, "num_hits", hits);
	add_assoc_long(return_value, "num_misses", misses);
	add_assoc_long(return_value, "num_inserts", inserts);
	add_assoc_long(return_value, "num_entries", entries);
	add_assoc_long(return_value, "expunges", expunges);
	add_assoc_long(return_value, "start_time", start);
	add_assoc_long(return_value, "mem_size", mem);
	add_assoc_string(return_value, "memory_type", "mmap");
	if (limited) {
		zval_ptr_dtor(&list);
	} else {
		add_assoc_zval(return_value, "cache_list", &list);
		zval deleted;
		array_init(&deleted);
		add_assoc_zval(return_value, "deleted_list", &deleted);
	}
}

PHP_FUNCTION(apcu_sma_info)
{
	bool limited = false;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(limited)
	ZEND_PARSE_PARAMETERS_END();

	if (!apc_on) {
		php_error_docref(NULL, E_WARNING, "No APC SMA info available.  Perhaps APC is disabled via apc.enabled?");
		RETURN_FALSE;
	}

	uint32_t run_at[STASH_MAX_SEGS], run_len[STASH_MAX_SEGS], runs = 0;
	arena_lock(); /* no allocation in here */
	double avail = (double) (arena_size - arena->header_size - arena->live_bytes);
	for (uint32_t i = 0, start = 0, n = 0; !limited && i <= arena->nsegs; i++) { /* adjacent free segments form one block */
		bool free_seg = i < arena->nsegs && (arena->segs[i].state == SEG_FREE || arena->segs[i].state == SEG_DEAD);
		if (free_seg && !n++) {
			start = i;
		} else if (!free_seg && n) {
			run_at[runs] = start;
			run_len[runs++] = n;
			n = 0;
		}
	}
	arena_unlock();

	zval blocks;
	array_init(&blocks);
	for (uint32_t r = 0; r < runs; r++) {
		zval row;
		array_init(&row);
		add_assoc_long(&row, "size", (zend_long) run_len[r] << seg_shift);
		add_assoc_long(&row, "offset", seg_start(run_at[r]) - (char *) arena);
		add_next_index_zval(&blocks, &row);
	}

	array_init(return_value);
	add_assoc_long(return_value, "num_seg", 1);
	add_assoc_double(return_value, "seg_size", (double) arena_size);
	add_assoc_double(return_value, "avail_mem", avail);
	if (limited) {
		zval_ptr_dtor(&blocks);
	} else {
		zval outer;
		array_init(&outer);
		add_next_index_zval(&outer, &blocks);
		add_assoc_zval(return_value, "block_lists", &outer);
	}
}

/* ---- module ---- */

static ZEND_INI_MH(OnBool)
{
	bool v = zend_ini_parse_bool(new_value);
	if (zend_string_equals_literal(entry->name, "apc.enabled")) {
		ini_enabled = v;
	} else if (zend_string_equals_literal(entry->name, "apc.enable_cli")) {
		ini_enable_cli = v;
	}
	return SUCCESS;
}

/* "auto" (default): zero-copy for the whole request only under SAPIs whose requests are short. */
static ZEND_INI_MH(OnRequestScope)
{
	if (zend_string_equals_literal_ci(new_value, "auto")) {
		static const char *const short_lived[] = {"fpm-fcgi", "cgi-fcgi", "apache2handler", "apache2filter", "litespeed", "fcgi", "cli-server", NULL};
		request_scope = false;
		for (int i = 0; short_lived[i]; i++) {
			request_scope |= strcmp(sapi_module.name, short_lived[i]) == 0;
		}
	} else {
		request_scope = zend_ini_parse_bool(new_value);
	}
	return SUCCESS;
}

static ZEND_INI_MH(OnShmSize)
{
	arena_size = (size_t) zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnHint)
{
	ini_hint = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnTtl)
{
	ini_ttl = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY("apc.enabled", "1", PHP_INI_SYSTEM, OnBool)
	PHP_INI_ENTRY("apc.enable_cli", "0", PHP_INI_SYSTEM, OnBool)
	PHP_INI_ENTRY("apc.shm_size", "32M", PHP_INI_SYSTEM, OnShmSize)
	PHP_INI_ENTRY("apc.entries_hint", "4096", PHP_INI_SYSTEM, OnHint)
	PHP_INI_ENTRY("apc.ttl", "0", PHP_INI_SYSTEM, OnTtl)
	PHP_INI_ENTRY("stash.request_scope", "auto", PHP_INI_SYSTEM, OnRequestScope)
PHP_INI_END()

PHP_MINIT_FUNCTION(stash)
{
	REGISTER_INI_ENTRIES();
	apc_on = ini_enabled && (ini_enable_cli || strcmp(sapi_module.name, "cli") != 0);

	/* ~2 slots per hinted entry, and at least one per 256 bytes of arena */
	uint32_t nslots = 1024;
	while ((zend_long) nslots < ini_hint * 2 || (size_t) nslots < arena_size / 256) {
		nslots <<= 1;
	}
	size_t header = (sizeof(stash_arena) + nslots * sizeof(slot_t) + 63) & ~(size_t) 63;
	seg_shift = 16;
	while ((arena_size >> seg_shift) > STASH_MAX_SEGS) {
		seg_shift++;
	}
	seg_size = (size_t) 1 << seg_shift;
	if (arena_size > ((size_t) 32 << 30) || arena_size < header + 4 * seg_size) {
		php_error_docref(NULL, E_CORE_ERROR, "Unable to allocate %zu bytes of shared memory for cache structures. Either apc.shm_size is too small or apc.entries_hint too large", header);
		return FAILURE;
	}
	void *p = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		return FAILURE;
	}
	arena = p; /* zero-filled by the kernel */
	arena->header_size = header;
	arena->nslots = nslots;
	slot_mask = nslots - 1;
	seg_base = (char *) arena + header;
	arena->nsegs = (arena_size - header) >> seg_shift;
	arena->nfree = arena->nsegs;
	for (int b = 0; b < TTL_BUCKETS; b++) {
		arena->cur[b] = -1;
	}
	arena->start_time = time(NULL);
	pthread_atfork(NULL, NULL, atfork_child);

	REGISTER_LONG_CONSTANT("APC_LIST_ACTIVE", APC_LIST_ACTIVE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_LIST_DELETED", APC_LIST_DELETED, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_TYPE", APC_ITER_TYPE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_KEY", APC_ITER_KEY, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_VALUE", APC_ITER_VALUE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_NUM_HITS", APC_ITER_NUM_HITS, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_MTIME", APC_ITER_MTIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_CTIME", APC_ITER_CTIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_DTIME", APC_ITER_DTIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_ATIME", APC_ITER_ATIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_REFCOUNT", APC_ITER_REFCOUNT, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_MEM_SIZE", APC_ITER_MEM_SIZE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_TTL", APC_ITER_TTL, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_NONE", 0, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_ALL", APC_ITER_ALL, CONST_PERSISTENT);

	iter_ce = register_class_APCUIterator(zend_ce_iterator);
	iter_ce->create_object = iter_create;
	memcpy(&iter_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	iter_handlers.offset = offsetof(stash_iter, std);
	iter_handlers.free_obj = iter_free;
	iter_handlers.clone_obj = NULL;
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(stash)
{
	if (my_slot >= 0) {
		bits_clear(my_slot);
		__atomic_store_n(&arena->workers[my_slot].pid, 0, __ATOMIC_SEQ_CST);
	}
	munmap(arena, arena_size);
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(stash)
{
	if (my_slot < 0) {
		my_slot = claim_worker();
	}
	scope_depth = request_scope ? 1 : 0;
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(stash)
{
	scope_depth = 0;
	return SUCCESS;
}

/* Pins go only after zend_deactivate: built-in modules (session) still read user values in their RSHUTDOWN, which runs after ours. */
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(stash)
{
	pin_refs = 0;
	keep_release();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(stash)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "stash support", "enabled");
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

zend_module_entry stash_module_entry = {
	STANDARD_MODULE_HEADER,
	"stash",
	ext_functions,
	PHP_MINIT(stash),
	PHP_MSHUTDOWN(stash),
	PHP_RINIT(stash),
	PHP_RSHUTDOWN(stash),
	PHP_MINFO(stash),
	PHP_STASH_VERSION,
	NO_MODULE_GLOBALS,
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(stash),
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_STASH
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(stash)
#endif
