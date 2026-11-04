/* Internal to the arena units (stash_arena.c, stash_evict.c, stash_lease.c, stash_owner.c) and tests/c/repair.c. Shard locked unless noted. */

#ifndef STASH_ARENA_INT_H
#define STASH_ARENA_INT_H

#include "stash_arena.h"

extern size_t stash_seg_size;
extern uint32_t *stash_ghosts[STASH_MAX_SHARDS]; /* per shard, as many as slots: tags of recently evicted keys (S3-FIFO ghost) */

static inline char *seg_start(uint32_t s) { return stash_seg_base + ((size_t) s << stash_seg_shift); }
static inline stash_slot slot_make(const stash_entry *e) { return (e->h << 32) | (uint32_t) (((const char *) e - (const char *) stash_a) >> 3); }
static inline stash_slot *tab_of(const stash_shard *sh) { return stash_tabs[sh - stash_a->shards]; }

/* Direct-mapped: a newer eviction overwrites an older one in its cell. Index: Fibonacci mix below the shard's top 8 bits
 * (short keys' DJB hashes have few high bits); tag: the low bits (never 0). */
static inline uint32_t *ghost_cell(const stash_shard *sh, uint64_t h)
{
	return &stash_ghosts[sh - stash_a->shards][(uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 24) & stash_tab_mask];
}
static inline uint32_t ghost_tag(uint64_t h) { return (uint32_t) h | 1; }

/* ---- owner ids (stash_owner.c): pid + process start time, so a recycled pid does not look like its dead predecessor ---- */
uint64_t stash_owner_self(void);
bool stash_owner_dead(uint64_t id); /* gone, a zombie, or its pid reused; reads /proc: no lock needed, better none held */

/* ---- segments, entries, slots (stash_arena.c) ---- */
int stash_ttl_bucket(time_t ttl);
/* reserve: may take the last free segment (kept back so a merge can always make progress). */
stash_entry *stash_seg_alloc(stash_shard *sh, size_t total, bool reserve, int bucket, time_t exp);
void stash_entry_birth(stash_shard *sh, stash_entry *e); /* allocated -> live */
void stash_entry_die(stash_shard *sh, stash_entry *e);   /* live -> dead (slot already gone or repointed) */
void stash_entry_unlink(stash_shard *sh, stash_entry *e);
void stash_abort(stash_shard *sh, stash_entry *e);       /* allocated, never published */
stash_slot *stash_slot_of(stash_shard *sh, const stash_entry *e);
stash_entry *stash_alloc(stash_shard *sh, size_t total, time_t exp, time_t now);
stash_put_result stash_commit(stash_shard *sh, stash_entry *e, bool add_only);

/* ---- expiry and merge (stash_evict.c) ---- */
bool stash_expire_step(stash_shard *sh, time_t now, int budget); /* true if it unlinked anything */
bool stash_merge(stash_shard *sh, bool evict_only);              /* false if there was nothing to merge */

#endif
