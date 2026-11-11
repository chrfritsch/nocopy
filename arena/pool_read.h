/* nocopy arena: the pool's reader side. A reader publishes the segments it may touch (pins) before dereferencing anything
 * in them; a segment is reused only when no worker pins it (ar_pool_pinned). Inline: the read path. */

#ifndef NOCOPY_ARENA_POOL_READ_H
#define NOCOPY_ARENA_POOL_READ_H

#include "proc.h"

/* Process-local copies of the pool's layout: read-only after the arena is opened, valid in forked children. */
AR_HIDDEN extern char *ar_seg_base;
AR_HIDDEN extern unsigned ar_seg_shift;
AR_HIDDEN extern uint32_t ar_nsegs;
AR_HIDDEN extern uint32_t ar_seg_words;  /* bitmap words in use: (nsegs + 63) / 64 */
AR_HIDDEN extern uint64_t *ar_pins;      /* per worker slot: the segments it pins (ar_pin_stride words) */
AR_HIDDEN extern uint64_t *ar_keeps;     /* per worker slot: the subset it keeps past a call (ar_keep_stride words) */
AR_HIDDEN extern uint32_t ar_keep_stride;
AR_HIDDEN extern uint32_t ar_pin_stride;
AR_HIDDEN extern uint32_t *ar_unpin_gen; /* bumped when a dead segment is still pinned: readers drop their idle pins */

static inline uint32_t ar_seg_of(const void *p) { return (uint32_t) (((const char *) p - ar_seg_base) >> ar_seg_shift); }
static inline uint64_t *ar_pins_of(int slot) { return ar_pins + (size_t) slot * ar_pin_stride; }
static inline uint64_t *ar_keeps_of(int slot) { return ar_keeps + (size_t) slot * ar_keep_stride; }

/* What this reader has pinned: per process (NTS) or thread (ZTS). */
typedef struct {
	int slot;           /* worker slot, -1 none */
	bool transient;     /* mine has bits that are not in keep */
	uint32_t unpin_seen; /* *ar_unpin_gen when the idle pins were last dropped */
	uint32_t idle;       /* pins taken since then (upper bound) */
	uint32_t nkeep;      /* segments in keep */
	int64_t keep_until;  /* monotonic ms after which nothing more is kept, 0 no limit */
	uint64_t mine[NOCOPY_SEG_WORDS]; /* published as pinned */
	uint64_t keep[NOCOPY_SEG_WORDS]; /* subset kept past the call: a zero-copy pointer escaped */
} ar_pins_t;

/* Publish "I may touch this segment" before dereferencing anything in it.
 * True if the pin is new: the caller must then re-check that what it found is still linked. */
static inline bool ar_pin(ar_pins_t *r, const void *p)
{
	uint32_t s = ar_seg_of(p), w = s >> 6;
	uint64_t b = ar_segbit(s);
	if (r->mine[w] & b) {
		return false;
	}
	r->mine[w] |= b;
	r->transient = true;
	r->idle++;
	__atomic_store_n(&ar_pins_of(r->slot)[w], r->mine[w], __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	AR_HOOK("pin.set");
	return true;
}

/* s newly kept: true once the arena's pin budget is spent or keep_until passed (pool.c). */
bool ar_pins_kept(ar_pins_t *r, uint32_t s);

/* A zero-copy pointer into p's segment leaves the call: keep it pinned until ar_pins_drop_all.
 * True: further fetches should copy. Kept pins block reclaim for everyone, so the budget is arena-wide. */
static inline bool ar_pin_keep(ar_pins_t *r, const void *p)
{
	uint32_t s = ar_seg_of(p);
	if (r->keep[s >> 6] & ar_segbit(s)) {
		return false;
	}
	return ar_pins_kept(r, s);
}

static inline void ar_pins_drop_transient(ar_pins_t *r)
{
	if (r->slot < 0 || !r->transient) {
		return;
	}
	for (uint32_t w = 0; w < ar_seg_words; w++) {
		if (r->mine[w] & ~r->keep[w]) {
			r->mine[w] &= r->keep[w];
			__atomic_store_n(&ar_pins_of(r->slot)[w], r->mine[w], __ATOMIC_SEQ_CST);
		}
	}
	r->transient = false;
	r->idle = 0;
}

/* Idle pins outlive the call (re-pinning costs a full fence per call); dropped once a writer asks, past nsegs/16, at request end.
 * ponytail: a process idle between calls holds up to nsegs/16 segments; revoking them would need the per-call fence back. */
static inline void ar_pins_drop_if_asked(ar_pins_t *r)
{
	uint32_t g = __atomic_load_n(ar_unpin_gen, __ATOMIC_RELAXED);
	if (r->transient && (g != r->unpin_seen || r->idle > (ar_nsegs >> 4))) {
		r->unpin_seen = g;
		ar_pins_drop_transient(r);
	}
}

/* About to wait: every pin not kept goes, idle or not. */
static inline void ar_pins_drop_idle(ar_pins_t *r)
{
	r->transient = r->slot >= 0;
	ar_pins_drop_transient(r);
}

/* Every pin goes, kept ones too: no earlier zero-copy result is used again. */
static inline void ar_pins_drop_all(ar_pins_t *r)
{
	for (uint32_t w = 0; r->nkeep && w < ar_seg_words; w++) {
		if (r->keep[w]) {
			__atomic_store_n(&ar_keeps_of(r->slot)[w], 0, __ATOMIC_RELAXED);
		}
	}
	memset(r->keep, 0, sizeof r->keep);
	r->nkeep = 0;
	r->transient = true;
	ar_pins_drop_transient(r);
}

#endif
