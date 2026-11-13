/* nocopy arena: the eviction policy. Decides what survives when a shard needs room: merge (Segcache: compaction and eviction
 * in one pass), read counts per entry and segment, and the ghost of recently evicted keys (S3-FIFO). The only place that
 * chooses victims; moving and unlinking entries is its caller's (ar_merge). Shard locked throughout. */

#ifndef NOCOPY_ARENA_POLICY_H
#define NOCOPY_ARENA_POLICY_H

#include "policy_read.h"

/* Per shard, in the shard. */
typedef struct {
	uint64_t evicted;    /* entries unlinked by merge while unexpired */
	uint64_t ghost_hits; /* new keys that were evicted recently: they start hot */
	uint64_t evacuated;  /* bytes moved by merges */
} ar_policy;

/* expunges: arena-wide, in the header (rounds that lost live entries, and clears). ghost_slots: cells per shard's ghost table. */
void ar_policy_adopt(uint64_t *expunges, uint32_t ghost_slots);
void ar_policy_count_expunge(void); /* before the unlinks: a reader that misses then sees it */

/* A new key: its starting hot (2 if it was evicted recently: survives two merges without a read). ghosts: its shard's table. */
uint8_t ar_policy_admit(ar_policy *pol, uint16_t *ghosts, uint64_t h);

/* e's reads join (leave) its segment's count. */
static inline __attribute__((always_inline)) void ar_policy_born(nocopy_entry *e)
{
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (f) {
		__atomic_add_fetch(&ar_segs[ar_seg_of(e)].reads, (1 << f) - 1, __ATOMIC_RELAXED);
	}
}
static inline __attribute__((always_inline)) void ar_policy_died(nocopy_entry *e)
{
	uint8_t f = __atomic_load_n(&e->hot, __ATOMIC_RELAXED);
	if (f) {
		__atomic_sub_fetch(&ar_segs[ar_seg_of(e)].reads, (1 << f) - 1, __ATOMIC_RELAXED);
	}
}

/* What a merge needs from its shard. */
typedef struct {
	ar_log *log;
	ar_policy *pol;
	uint16_t *ghosts;
	uint64_t last_seq; /* merge order for a run sent to the back */
	/* old copied forward into its bucket with `hot`, slot repointed, old died. False: no room left. */
	bool (*relocate)(nocopy_entry *old, uint8_t hot, void *arg);
	void (*unlink)(nocopy_entry *e, void *arg); /* slot removed, then ar_log_died */
	void *arg;
} ar_merge;

/* One round: compacts sparse segments losslessly, else evicts (ar_policy_evict). False if there was nothing to merge.
 * Victims that readers pin die but stay allocated until unpinned; the next round moves on. */
bool ar_policy_merge(const ar_merge *m);
/* One round that evicts the oldest segments, keeping their most-read entries: for a full slot table, where compaction frees no slot. */
bool ar_policy_evict(const ar_merge *m);

#endif
