/* nocopy arena: the segment pool. Segments are the arena's unit of memory: a free one belongs to no tenant (shard);
 * a tenant claims runs of them, and gives them back zeroed once no worker pins them. owner[] is the truth of who holds what. */

#ifndef NOCOPY_ARENA_POOL_H
#define NOCOPY_ARENA_POOL_H

#include "pool_read.h"

#define AR_SEG_MIN_SHIFT 16 /* 64 KB */
#define AR_SEG_MAX_SHIFT 22 /* segments grow to 4 MB, then their number: an eviction round moves up to one */

/* In the arena header. Offsets are from ar_base. */
typedef struct {
	uint32_t nsegs, shift, pin_stride; /* pin_stride: words per worker's bitmap, a multiple of 8 (own cache lines) */
	size_t segs_off;  /* the first segment */
	size_t owner_off; /* uint8_t per segment: tenant, 0 free */
	size_t meta_off;  /* ar_segmeta per segment */
	size_t pins_off;  /* NOCOPY_MAX_WORKERS bitmaps */
	uint32_t unpin_gen __attribute__((aligned(64)));
} ar_pool_hdr;

/* What the pool keeps per segment besides its owner; reset when the segment is given back. */
typedef struct {
	uint32_t gen; /* bumped on every give: a cursor that stopped in a segment notices its reuse */
	uint8_t deps; /* bits its contents hold on something outside (atoms' halves); OR only, while the tenant's lock is held */
} ar_segmeta;

AR_HIDDEN extern size_t ar_seg_size;
AR_HIDDEN extern uint8_t *ar_seg_owner;
AR_HIDDEN extern ar_segmeta *ar_segmeta_at;

/* Bytes behind the header for maxsegs segments, at 64-byte aligned offsets from `at`: owner, meta, pins. Returns the end. */
size_t ar_pool_plan(ar_pool_hdr *h, size_t at, size_t maxsegs);
void ar_pool_init(ar_pool_hdr *h, size_t segs_off, uint32_t nsegs, unsigned shift); /* zero-filled memory, after ar_pool_plan */
void ar_pool_adopt(ar_pool_hdr *h);

static inline char *ar_seg_start(uint32_t s) { return ar_seg_base + ((size_t) s << ar_seg_shift); }
static inline uint8_t ar_pool_tenant(uint32_t s) { return __atomic_load_n(&ar_seg_owner[s], __ATOMIC_ACQUIRE); }
static inline uint32_t ar_seg_gen(uint32_t s) { return ar_segmeta_at[s].gen; }
static inline uint8_t ar_seg_deps(uint32_t s) { return ar_segmeta_at[s].deps; }
static inline void ar_seg_deps_or(uint32_t s, uint8_t bits) { ar_segmeta_at[s].deps |= bits; }

uint32_t ar_pool_free_count(void);
/* Claims k contiguous free segments for tenant, searching from `from` on, as long as keep_free others stay free. AR_NONE if none. */
uint32_t ar_pool_take(uint8_t tenant, uint32_t k, uint32_t from, uint32_t keep_free);
/* Gives segments s..s+k-1 back: their tenant has dropped its records of them. Resets their meta; poisons them under NOCOPY_POISON. */
void ar_pool_give(uint32_t s, uint32_t k);
/* Any segment's deps has one of bits. The caller holds every tenant's lock: deps change only under them. */
bool ar_pool_any_deps(uint8_t bits);

/* After `dead` (segments their tenant found unused) was collected: those some live worker pins go into held (cleared first).
 * A worker found dead has its pins cleared. Issues the fence that orders the collection before the pin reads. */
void ar_pool_pinned(const uint64_t *dead, uint64_t *held, time_t now);
void ar_pool_ask_unpin(void); /* readers drop their idle pins at their next call end */
uint32_t ar_pool_pinned_count(void);

void ar_pins_clear(int slot);                /* a slot claimed or released */
void ar_pins_publish(const ar_pins_t *r);    /* a forked child's new slot: the parent's pins it inherited */

#endif
