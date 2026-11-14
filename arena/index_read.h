/* nocopy arena: a shard's slot table, reader side. Linear probing over fixed tables; slots hold a tag and an offset, never
 * dereferenced here. A lock-free probe that misses while a delete shifts slots back retries (mseq, a seqlock). Inline: the read path. */

#ifndef NOCOPY_ARENA_INDEX_READ_H
#define NOCOPY_ARENA_INDEX_READ_H

#include "base.h"

/* low 32 bits: arena offset / 8, high 32: ar_slot_tag(hash) (its low bits are the home slot; probe filter); 0 empty */
typedef uint64_t ar_slot;

/* Mixed: raw DJB low bits of `prefix.N` keys built probe runs of hundreds. Own multiplier keeps it independent of the shard; tests/018 mirrors it. */
static inline uint32_t ar_slot_tag(uint64_t h) { return (uint32_t) ((h * 0xC2B2AE3D27D4EB4FULL) >> 32); }
static inline ar_slot ar_slot_make(uint32_t tag, uint32_t off8) { return (uint64_t) tag << 32 | off8; }
static inline uint32_t ar_slot_off8(ar_slot s) { return (uint32_t) s; }

/* In the shard, written under its lock. */
typedef struct {
	uint32_t nlive, cap; /* live slots, and the most the table may hold (3/4): an empty slot ends every run */
	uint32_t mseq __attribute__((aligned(64))); /* odd while a delete shifts slots: a reader's miss is retried */
	uint32_t nalt; /* linked entries under their alternative hash (ar_alt_hash); 0: a miss looks nowhere else. On mseq's line: a miss reads both */
} ar_index_hdr;

/* The arena's secret key (from its header, random at creation): what crafted keys cannot collide on. */
AR_HIDDEN extern uint64_t ar_seed[2];
uint64_t ar_siphash(const char *k, size_t n); /* SipHash-1-3 under ar_seed */
/* A key's alternative hash: ar_siphash moved into part (ar_part's top bits), where a key goes that would join ALT_AFTER keys of its hash. */
uint64_t ar_alt_hash(uint32_t part, const char *k, uint32_t klen);

/* A shard's table as this process sees it: fixed for the arena's life, so readers touch no header line writers write. */
typedef struct {
	ar_slot *t;
	ar_index_hdr *hdr;
	uint32_t mask;
} ar_index;

/* Candidates for a hash, in probe order. A writer (lock held) ignores seq; after the last candidate, at is the empty slot ending the run. */
typedef struct {
	ar_slot *t;
	const uint32_t *mseq;
	uint32_t mask, tag, i, n, at, seq;
} ar_probe;

static inline __attribute__((always_inline)) void ar_probe_restart(ar_probe *p)
{
	p->seq = __atomic_load_n(p->mseq, __ATOMIC_ACQUIRE);
	p->i = p->tag & p->mask;
	p->n = 0;
}

static inline __attribute__((always_inline)) void ar_probe_start(ar_probe *p, const ar_index *ix, uint64_t h)
{
	p->t = ix->t;
	p->mseq = &ix->hdr->mseq;
	p->mask = ix->mask;
	p->tag = ar_slot_tag(h);
	ar_probe_restart(p);
}

/* A writer's probe: its shard's lock is held, so no seqlock and plain loads. */
static inline __attribute__((always_inline)) void ar_probe_start_locked(ar_probe *p, const ar_index *ix, uint64_t h)
{
	p->t = ix->t;
	p->mseq = NULL;
	p->mask = ix->mask;
	p->tag = ar_slot_tag(h);
	p->i = p->tag & p->mask;
	p->n = 0;
	p->seq = 0;
	p->at = p->i; /* the loop always sets it: for gcc */
}

static inline __attribute__((always_inline)) ar_slot ar_probe_step(ar_probe *p, bool locked)
{
	while (p->n <= p->mask) {
		uint32_t i = p->i;
		if (!locked) {
			AR_HOOK("probe.load");
		}
		ar_slot s = locked ? p->t[i] : __atomic_load_n(&p->t[i], __ATOMIC_ACQUIRE);
		p->at = i;
		p->i = (i + 1) & p->mask;
		p->n++;
		if (!s) {
			p->n = p->mask + 1;
			return 0;
		}
		if ((uint32_t) (s >> 32) == p->tag) {
			return s;
		}
	}
	return 0;
}

/* The next slot carrying the hash's tag, or 0 at the end of its run. */
static inline __attribute__((always_inline)) ar_slot ar_probe_next(ar_probe *p) { return ar_probe_step(p, false); }
static inline __attribute__((always_inline)) ar_slot ar_probe_next_locked(ar_probe *p) { return ar_probe_step(p, true); }

static inline ar_slot *ar_probe_slot(const ar_probe *p) { return &p->t[p->at]; }

/* The candidate is still linked: re-checked after pinning what it points to. */
static inline __attribute__((always_inline)) bool ar_probe_still(const ar_probe *p, ar_slot s) { return __atomic_load_n(&p->t[p->at], __ATOMIC_ACQUIRE) == s; }

/* A reader's miss counts only if no delete shifted slots meanwhile: one may move the key backwards past the probe. Else it starts over.
 * No fence: the slot loads were acquire loads, so this one cannot move before them. */
static inline __attribute__((always_inline)) bool ar_probe_miss_final(ar_probe *p)
{
	if (!(p->seq & 1) && __atomic_load_n(p->mseq, __ATOMIC_RELAXED) == p->seq) {
		return true;
	}
	AR_HOOK_ON("probe.retry", p);
	ar_cpu_relax();
	ar_probe_restart(p);
	return false;
}

#endif
