/* nocopy arena: limits and primitives every arena module shares. Plain C, no PHP.
 * Naming: nocopy_* is the arena's API (nocopy_arena.h), ar_* its modules' internals; code outside arena/ uses no ar_*. */

#ifndef NOCOPY_ARENA_BASE_H
#define NOCOPY_ARENA_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Process-local state the read path loads: hidden, so no GOT load. */
#define AR_HIDDEN __attribute__((visibility("hidden")))

/* Test hooks (tests/c, -DNOCOPY_HOOKS): ar_hook runs at a named step, where a test dies or runs another worker's step.
 * AR_HOOK_ON: a step that waits, and for what (ar_hook_on: the lock, the probe). */
#ifdef NOCOPY_HOOKS
extern void (*ar_hook)(const char *point);
extern const void *ar_hook_on;
# define AR_HOOK(point) do { if (ar_hook) { ar_hook(point); } } while (0)
# define AR_HOOK_ON(point, obj) do { if (ar_hook) { ar_hook_on = (obj); ar_hook(point); } } while (0)
#else
# define AR_HOOK(point) ((void) 0)
# define AR_HOOK_ON(point, obj) ((void) 0)
#endif

/* Invariant checks in test and debug builds (tests/c's -DNOCOPY_POISON, ./dev asan's -DNOCOPY_DEBUG): a failure aborts. */
#if defined(NOCOPY_POISON) || defined(NOCOPY_DEBUG)
# define AR_CHECKS 1
__attribute__((noreturn, cold)) void ar_assert_fail(const char *what, const char *file, int line);
# define AR_ASSERT(c) do { if (!(c)) { ar_assert_fail(#c, __FILE__, __LINE__); } } while (0)
#else
# define AR_CHECKS 0
# define AR_ASSERT(c) ((void) 0)
#endif

#define NOCOPY_MAX_WORKERS 1024 /* ponytail: more live processes/threads than this cannot claim a slot */
#define NOCOPY_MAX_SEGS 8192
#define NOCOPY_SEG_WORDS (NOCOPY_MAX_SEGS / 64)
#define NOCOPY_MAX_SHARDS 8
#define NOCOPY_USER_BYTES 256 /* the user's shared state in the arena header */
#define NOCOPY_ALIGN(n) (((n) + 7) & ~(size_t) 7)
#define AR_NONE UINT32_MAX

/* Where this process maps the arena (the same address in every process), and its size; NULL: no arena.
 * Slots and worker records hold offsets from it in units of 8 bytes. */
AR_HIDDEN extern char *ar_base;
AR_HIDDEN extern size_t ar_size;

/* Fibonacci hashing: spreads a key hash's bits into the high ones, which callers take as an index. */
static inline uint64_t ar_fib_mix(uint64_t h) { return h * 0x9E3779B97F4A7C15ULL; }

/* Which of mask + 1 parts (shards, lease tables) a key hash belongs to: top bits. */
static inline uint32_t ar_part(uint64_t h, uint32_t mask) { return (uint32_t) (ar_fib_mix(h) >> 56) & mask; }

static inline uint32_t ar_off8(const void *p) { return (uint32_t) (((const char *) p - ar_base) >> 3); }
static inline void *ar_at8(uint32_t off8) { return ar_base + ((size_t) off8 << 3); }

/* Outcome of a call that can fail for a reason its caller reports. */
typedef enum { NOCOPY_ERR_NONE, NOCOPY_ERR_TOO_SMALL, NOCOPY_ERR_FAILED } nocopy_errc;
typedef struct {
	nocopy_errc code;
	const char *msg; /* NULL with NOCOPY_ERR_NONE */
	size_t need;     /* NOCOPY_ERR_TOO_SMALL: bytes the layout needs */
} nocopy_err;

static inline void ar_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
}

/* Segment sets: bitmaps of NOCOPY_SEG_WORDS words (pins, ownership, reclaim's sets). */
static inline uint64_t ar_segbit(uint32_t s) { return (uint64_t) 1 << (s & 63); }
static inline uint32_t ar_segs_count(const uint64_t *m, uint32_t nwords)
{
	uint32_t n = 0;
	for (uint32_t w = 0; w < nwords; w++) {
		n += (uint32_t) __builtin_popcountll(m[w]);
	}
	return n;
}
static inline bool ar_segs_has(const uint64_t *m, uint32_t s) { return m[s >> 6] >> (s & 63) & 1; }
static inline void ar_segs_add(uint64_t *m, uint32_t s) { m[s >> 6] |= ar_segbit(s); }
static inline void ar_segs_del(uint64_t *m, uint32_t s) { m[s >> 6] &= ~ar_segbit(s); }

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

#endif
