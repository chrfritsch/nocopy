/* nocopy arena: the eviction policy's reader side. Inline: the read path. */

#ifndef NOCOPY_ARENA_POLICY_READ_H
#define NOCOPY_ARENA_POLICY_READ_H

#include "log.h"

static inline uint32_t ar_rand(uint32_t *rng)
{
	uint32_t x = *rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return *rng = x;
}

/* A read of e: a Morris-style counter in the header. The first read sets 1, later ones add 1 with probability 1/2^hot,
 * so a hot key writes its cache line ~log2(reads) times between merges and it stays shared otherwise. Its segment's reads follow. */
static inline __attribute__((always_inline)) void ar_policy_hit(uint32_t *rng, nocopy_entry *e)
{
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (f < NOCOPY_HOT_MAX && (!f || !(ar_rand(rng) & ((1u << f) - 1)))) {
		__atomic_store_n(&e->hot, f + 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&ar_segs[ar_seg_of(e)].reads, 1 << f, __ATOMIC_RELAXED);
	}
}

#endif
