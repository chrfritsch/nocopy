/* nocopy arena: a shard's slot table, writer side (its shard's lock held). Sized once; deletes shift later entries of the run
 * back (no tombstones). The table knows offsets only: comparing keys is its caller's job. */

#ifndef NOCOPY_ARENA_INDEX_H
#define NOCOPY_ARENA_INDEX_H

#include "index_read.h"

void ar_index_init(ar_index_hdr *h, uint32_t slots); /* zero-filled memory */
void ar_index_adopt(ar_index *ix, ar_index_hdr *h, ar_slot *t, uint32_t slots);

static inline uint32_t ar_index_nlive(const ar_index *ix) { return __atomic_load_n(&ix->hdr->nlive, __ATOMIC_RELAXED); }
static inline bool ar_index_full(const ar_index *ix) { return ix->hdr->nlive >= ix->hdr->cap; }
static inline ar_slot ar_index_load(const ar_index *ix, uint32_t i) { return __atomic_load_n(&ix->t[i], __ATOMIC_ACQUIRE); }

/* Publishes s at `at` (a probe's slot: the key's own, or the empty one ending its run). Release: a reader sees the block it points to. */
static inline void ar_index_put(ar_index *ix, ar_slot *at, ar_slot s)
{
	ix->hdr->nlive += !*at;
	__atomic_store_n(at, s, __ATOMIC_RELEASE);
}

void ar_index_remove(ar_index *ix, ar_slot *at);
ar_slot *ar_index_find(const ar_index *ix, ar_slot v); /* the slot holding exactly v, or NULL */
/* Empties the table; gone(s) for every slot that held something. */
void ar_index_clear(ar_index *ix, void (*gone)(ar_slot s, void *arg), void *arg);
/* After a holder died: an odd mseq made even, a key a shift copied but did not clear loses its second slot, nlive recounted. */
void ar_index_repair(ar_index *ix);

#endif
