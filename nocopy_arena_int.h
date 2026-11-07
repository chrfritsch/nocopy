/* Internal to the arena units (nocopy_arena.c, nocopy_evict.c, nocopy_lease.c, nocopy_owner.c) and tests/c/repair.c. Shard locked unless noted. */

#ifndef NOCOPY_ARENA_INT_H
#define NOCOPY_ARENA_INT_H

#include "nocopy_arena.h"

extern size_t nocopy_seg_size;
extern uint32_t nocopy_fair_segs; /* nsegs / nshards: what a shard may reclaim from others, and the largest entry (a division on every write cost store int ~3 ns) */
extern uint32_t *nocopy_ghosts[NOCOPY_MAX_SHARDS]; /* per shard, as many as slots: tags of recently evicted keys (S3-FIFO ghost) */

static inline char *seg_start(uint32_t s) { return nocopy_seg_base + ((size_t) s << nocopy_seg_shift); }
static inline nocopy_slot slot_make(const nocopy_entry *e) { return (uint64_t) nocopy_slot_tag(e->h) << 32 | (uint32_t) (((const char *) e - (const char *) nocopy_a) >> 3); }
static inline nocopy_slot *tab_of(const nocopy_shard *sh) { return nocopy_tabs[sh->idx]; }
static inline bool owns(const nocopy_shard *sh, uint32_t s) { return nocopy_segs_has(sh->own, s); }

/* The first segment >= s that sh owns, or UINT32_MAX. */
static inline uint32_t own_next(const nocopy_shard *sh, uint32_t s)
{
	if (s >= nocopy_a->nsegs) {
		return UINT32_MAX;
	}
	uint32_t w = s >> 6;
	uint64_t b = sh->own[w] & (~(uint64_t) 0 << (s & 63));
	while (!b) {
		if (++w == nocopy_seg_words) {
			return UINT32_MAX;
		}
		b = sh->own[w];
	}
	return w * 64 + (uint32_t) __builtin_ctzll(b);
}
#define SH_SEGS(sh, s) for (uint32_t s = own_next(sh, 0); s != UINT32_MAX; s = own_next(sh, s + 1))

/* Direct-mapped: a newer eviction overwrites an older one in its cell. Index: Fibonacci mix below the shard's top 8 bits
 * (short keys' DJB hashes have few high bits); tag: the low bits (never 0). */
static inline uint32_t *ghost_cell(const nocopy_shard *sh, uint64_t h)
{
	return &nocopy_ghosts[sh->idx][(uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 24) & nocopy_tab_mask];
}
static inline uint32_t ghost_tag(uint64_t h) { return (uint32_t) h | 1; }

/* ---- owner ids (nocopy_owner.c): pid + process start time, so a recycled pid does not look like its dead predecessor ---- */
uint64_t nocopy_owner_self(void);
bool nocopy_owner_dead(uint64_t id); /* gone, a zombie, or its pid reused; reads /proc: no lock needed, better none held */

/* ---- segments, entries, slots (nocopy_arena.c) ---- */
/* reserve: may take the last free segment (kept back so a merge can always make progress). */
nocopy_entry *nocopy_seg_alloc(nocopy_shard *sh, size_t total, bool reserve, int bucket, time_t exp);
void nocopy_entry_birth(nocopy_shard *sh, nocopy_entry *e); /* allocated -> live */
void nocopy_entry_die(nocopy_shard *sh, nocopy_entry *e);   /* live -> dead (slot already gone or repointed) */
void nocopy_entry_unlink(nocopy_shard *sh, nocopy_entry *e);
void nocopy_abort(nocopy_shard *sh, nocopy_entry *e);       /* allocated, never published */
nocopy_slot *nocopy_slot_of(nocopy_shard *sh, const nocopy_entry *e);
nocopy_entry *nocopy_alloc(nocopy_shard *sh, size_t total, time_t exp, time_t now);
nocopy_put_result nocopy_commit(nocopy_shard *sh, nocopy_entry *e, bool add_only);

/* ---- expiry and merge (nocopy_evict.c) ---- */
bool nocopy_expire_step(nocopy_shard *sh, time_t now, int budget); /* true if it unlinked anything */
bool nocopy_merge(nocopy_shard *sh, bool evict_only);              /* false if there was nothing to merge */

#endif
