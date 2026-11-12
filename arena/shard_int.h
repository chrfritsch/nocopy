/* nocopy arena: what the shard's two units share: shard.c (locks, writes, repair) and room.c (making room). Nobody else
 * includes it (scripts/check-layers.sh). Shard locked unless noted. */

#ifndef NOCOPY_ARENA_SHARD_INT_H
#define NOCOPY_ARENA_SHARD_INT_H

#include "shard.h"

void ar_shard_lock(ar_shard *sh); /* a dead holder's lock is taken over and the shard repaired */
void ar_shard_unlock(ar_shard *sh);

static inline ar_index *ix_of(const ar_shard *sh) { return &ar_ix[sh->idx]; }
static inline nocopy_entry *slot_entry(ar_slot s) { return ar_at8(ar_slot_off8(s)); }
static inline ar_slot slot_for(const nocopy_entry *e) { return ar_slot_make(ar_slot_tag(e->h), ar_off8(e)); }

/* pending -> live; e's reads join its segment's count */
static inline __attribute__((always_inline)) void birth(ar_shard *sh, nocopy_entry *e)
{
	ar_log_born(&sh->log, e);
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (f) {
		__atomic_add_fetch(&ar_segs[ar_seg_of(e)].reads, (1 << f) - 1, __ATOMIC_RELAXED);
	}
}

/* live -> dead (its slot already gone or repointed); e's reads leave its segment's count */
static inline __attribute__((always_inline)) void die(ar_shard *sh, nocopy_entry *e)
{
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (f) {
		__atomic_sub_fetch(&ar_segs[ar_seg_of(e)].reads, (1 << f) - 1, __ATOMIC_RELAXED);
	}
	ar_log_died(&sh->log, e);
}

/* ---- room.c ---- */

void ar_room_adopt(uint32_t nshards, uint16_t *ghosts, uint32_t slots_per, uint64_t *expunges, nocopy_rebase_fn rebase);
/* A block of total bytes, freeing room as needed, cheapest first. NULL: no room (or bigger than a shard's share).
 * May drop and retake the lock (rebalance): whatever the caller read of the shard before calling is stale afterwards. */
nocopy_entry *ar_room_alloc(ar_shard *sh, size_t total, time_t exp, time_t now);
bool ar_room_slot(ar_shard *sh);              /* the slot table is full: evicts until a slot is free; false if none could be */
uint8_t ar_room_admit(ar_shard *sh, uint64_t h); /* a new key's starting hot: 2 if it was evicted recently */
void ar_room_count_expunge(void);             /* before the unlinks: a reader that misses then sees it */

#endif
