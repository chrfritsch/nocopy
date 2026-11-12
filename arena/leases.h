/* nocopy arena: single-flight leases. Who computes a key hash right now: a fixed table per shard index with a lock of its own,
 * apart from the shard's (no cache entry, no slot). A dead or expired holder is taken over. */

#ifndef NOCOPY_ARENA_LEASES_H
#define NOCOPY_ARENA_LEASES_H

#include "ctx.h"

#define NOCOPY_LEASES 32 /* per table: concurrent single-flight computes */

/* Free when owner is 0 or until has passed. */
typedef struct {
	uint64_t h, owner;
	time_t until;
	int32_t slot;
} ar_lease;

typedef struct {
	ar_lock lock __attribute__((aligned(64)));
	ar_lease l[NOCOPY_LEASES];
} ar_lease_table;

void ar_leases_adopt(ar_lease_table *tables, uint32_t ntables); /* a power of two; a hash picks its table with ar_part */
uint64_t ar_leases_held(time_t now);

/* ---- API (nocopy_arena.h) ---- */

enum { NOCOPY_LEASE_TAKEN, NOCOPY_LEASE_HELD, NOCOPY_LEASE_MINE, NOCOPY_LEASE_FULL };
/* MINE: recursion, our own callback asks for its key. FULL: the table is full, compute without one. Two keys with the same hash share one. */
int nocopy_lease_take(nocopy_ctx *c, uint64_t h, time_t ttl);
void nocopy_lease_drop(nocopy_ctx *c, uint64_t h); /* only if c's slot still holds it: nobody drops a lease another worker retook */

#endif
