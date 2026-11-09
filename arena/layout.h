/* nocopy arena: what lies where in the region. Each module sizes its own part; this plans them, sets them up in a fresh
 * region, and points each module's process-local view at its part.
 *
 * [ header | slot tables | ghosts | segment records | pool: owners, meta, pins | atoms ][ segment 0 ][ segment 1 ] ... */

#ifndef NOCOPY_ARENA_LAYOUT_H
#define NOCOPY_ARENA_LAYOUT_H

#include "atoms.h"
#include "leases.h"
#include "region.h"
#include "shard.h"

typedef struct {
	ar_region_hdr region; /* first: the region's */
	size_t header_size;   /* where segments start */
	uint32_t nshards, slots_per;
	size_t tables_off, ghosts_off, segrec_off;
	time_t start_time;
	uint64_t expunges;
	ar_pool_hdr pool;
	ar_atoms_hdr atoms;
	ar_lease_table leases[NOCOPY_MAX_SHARDS];
	ar_shard shards[NOCOPY_MAX_SHARDS];
	char user[NOCOPY_USER_BYTES] __attribute__((aligned(64))); /* the user's shared state, zeroed at creation */
	ar_procs procs __attribute__((aligned(64)));
} ar_arena;

typedef struct {
	size_t size, header, tables_off, ghosts_off, segrec_off;
	unsigned shift;
	uint32_t nshards, slots_per;
	ar_pool_hdr pool;
	ar_atoms_hdr atoms;
} ar_layout;

AR_HIDDEN extern ar_arena *ar_hdr;

/* Segments: a power of two, at least 64 KB, growing until there are at most 256 but not past 4 MB (above 1G their number grows:
 * an eviction round moves up to a segment's worth under the shard lock). Shards only where each keeps >= 8 MB and >= 16 segments.
 * Slots: ~2 per hinted entry and at least one per 256 bytes of arena, fixed for its life. */
nocopy_err ar_layout_plan(ar_layout *l, size_t size, uint64_t hint);
void ar_layout_init(ar_arena *a, const ar_layout *l); /* zero-filled memory, at its final address */
/* This process's views of a's parts. False: the header is corrupt. */
bool ar_layout_adopt(ar_arena *a, nocopy_rebase_fn rebase);

#endif
