/* nocopy arena: the shared-memory store without PHP. The only arena header code outside arena/ includes, and nocopy_* the
 * only names it uses: the ar_* internals it pulls in are there for the inline read path (scripts/check-layers.sh).
 *
 * An entry is a header (nocopy_entry) plus the caller's block. Reads are lock-free and inline; every call that writes takes
 * its key's shard lock itself. The one callback the arena needs from its user is nocopy_rebase_fn (merge moved a block). */

#ifndef NOCOPY_ARENA_H
#define NOCOPY_ARENA_H

#include "atoms.h"
#include "ctx.h"
#include "leases.h"
#include "policy_read.h"
#include "region.h"
#include "shard.h"

/* ---- the arena ---- */

typedef struct {
	const char *name;  /* the POSIX shm name, e.g. /nocopy: processes opening it with the same abi share it */
	size_t size;       /* a creator's size; an attacher gets the creator's */
	uint64_t hint;     /* entries the slot tables should hold */
	uint64_t abi;      /* the user's layout id: another value is another arena */
	nocopy_rebase_fn rebase;
} nocopy_config;

/* Creates or attaches. Unused arenas of other builds under the name are removed. Not NOCOPY_ERR_NONE: no arena. */
nocopy_err nocopy_arena_open(const nocopy_config *cfg, nocopy_named *out);
void nocopy_arena_unhold(void);  /* the Valkey listener maps the arena without counting as its user */
bool nocopy_arena_current(void); /* false: the arena's name was removed or names another object now */
void nocopy_arena_close(void);

static inline bool nocopy_attached(void) { return ar_base != NULL; }
static inline size_t nocopy_arena_size(void) { return ar_size; }
static inline bool nocopy_in_arena(const void *p) { return (uintptr_t) p - (uintptr_t) ar_base < ar_size; }
/* NOCOPY_USER_BYTES of the user's shared state, zeroed at creation, at the same address in every process. */
AR_HIDDEN extern char *ar_user;
static inline void *nocopy_user_area(void) { return ar_user; }
/* Replaces the wall clock's seconds (TTLs, expiry) in this process; NULL: the wall clock again. For tests. */
void nocopy_set_clock(nocopy_clock_fn fn);

/* ---- a process's (thread's) handle: nocopy_ctx_init, then claim ---- */

bool nocopy_claim(nocopy_ctx *c);   /* false: no free worker slot */
void nocopy_release(nocopy_ctx *c); /* end of this process/thread */
bool nocopy_after_fork(nocopy_ctx *c); /* false: no worker slot (or no owner id) in the child */
void nocopy_request_end(nocopy_ctx *c); /* every pin goes, also of calls a bailout left open */

/* Reads happen between borrow_begin and borrow_end; what they found stays pinned until then (or longer: nocopy_keep). */
static inline void nocopy_borrow_begin(nocopy_ctx *c) { c->pin_refs++; }
static inline void nocopy_borrow_end(nocopy_ctx *c)
{
	if (--c->pin_refs == 0) {
		ar_pins_drop_if_asked(&c->pins);
	}
}
/* About to wait (FrankenPHP worker between requests): drop every pin not kept, idle or not. */
static inline void nocopy_drop_idle(nocopy_ctx *c) { ar_pins_drop_idle(&c->pins); }
/* A zero-copy pointer into p's segment leaves the call: kept pinned until nocopy_request_end. True: the caller should copy from now on. */
static inline bool nocopy_keep(nocopy_ctx *c, const void *p) { return ar_pin_keep(&c->pins, p); }
/* nocopy_keep answers true for keeps after this monotonic ms (nocopy_mono_ms), 0 never. */
static inline void nocopy_keep_until(nocopy_ctx *c, int64_t ms) { c->pins.keep_until = ms; }

/* This worker's xorshift: uniform 32 bits, no syscall. */
static inline uint32_t nocopy_rand(nocopy_ctx *c) { return ar_rand(&c->rng); }

/* A lookup's outcome in this worker's counters (nocopy_stats). Single writer per slot: no RMW. */
static inline void nocopy_count(nocopy_ctx *c, bool hit)
{
	ar_worker *w = ar_worker_at((uint32_t) c->pins.slot);
	uint64_t *n = hit ? &w->hits : &w->misses;
	__atomic_store_n(n, *n + 1, __ATOMIC_RELAXED);
}

/* ---- reads: lock-free, between borrow_begin/end ---- */

/* Pin first, then re-check the slot, only then dereference. Forced inline: gcc outlined it once fetch() got more callers (+9 ns copy fetch).
 * stale: also return an entry that expired, if nocopy_stale_ok (constant at every call site: folds away). */
static inline __attribute__((always_inline)) nocopy_entry *nocopy_find_ex(nocopy_ctx *c, uint64_t h, const char *k, uint32_t klen, bool stale)
{
	ar_probe p;
	ar_probe_start(&p, &ar_ix[ar_shard_idx(h)], h);
	for (;;) {
		ar_slot s;
		while ((s = ar_probe_next(&p))) {
			nocopy_entry *e = (nocopy_entry *) ar_at8(ar_slot_off8(s));
			if (ar_pin(&c->pins, e) && !ar_probe_still(&p, s)) {
				ar_probe_restart(&p); /* unlinked while we were pinning: the memory may be reused */
				continue;
			}
			if (e->h == h && e->klen == klen && nocopy_key_eq(nocopy_key(e), k, klen)) {
				if (nocopy_expired(e)) {
					return stale && nocopy_stale_ok(e) ? e : NULL;
				}
				ar_policy_hit(&c->rng, e);
				return e;
			}
		}
		if (ar_probe_miss_final(&p)) {
			return NULL;
		}
	}
}

static inline __attribute__((always_inline)) nocopy_entry *nocopy_find(nocopy_ctx *c, uint64_t h, const char *k, uint32_t klen)
{
	return nocopy_find_ex(c, h, k, klen, false);
}

/* fn(e) for each live, unexpired entry, which stays pinned until borrow_end. Lock-free.
 * ponytail: an entry a concurrent delete shifts backwards may be missed; a snapshot under all shard locks if that matters. */
void nocopy_each(nocopy_ctx *c, void (*fn)(const nocopy_entry *e, void *arg), void *arg);

/* ---- writes: nocopy_put_begin/commit/abort, nocopy_delete, nocopy_update, nocopy_clear (shard.h) ---- */
/* ---- single flight: nocopy_lease_take/drop (leases.h) ---- */

/* ---- atoms: strings stored once for the whole arena (atoms.h, atoms_read.h) ---- */

/* Announces the current generation in c's worker slot (nested calls share the outer view); false: no worker slot, no atoms. */
static inline bool nocopy_atom_begin(nocopy_ctx *c, nocopy_atom_view *v) { return ar_atom_begin(&c->atom, c->pins.slot, v); }
static inline void nocopy_atom_end(nocopy_ctx *c) { ar_atom_end(&c->atom, c->pins.slot); }

/* ---- stats ---- */

typedef struct {
	uint64_t used, entries, evacuated, hits, misses, evicted, expired, ghost_hits, expunges, lock_steals, slots, leases;
	uint64_t atoms, atoms_used, atoms_size, atom_rotations, shared_arrays, shared_arrays_used;
	uint64_t segment_size;
	uint32_t segments, free_segments, dead_segments, pinned_segments, kept_segments, shards;
	time_t start_time;
} nocopy_stats_t;
void nocopy_stats(nocopy_stats_t *s);
uint64_t nocopy_expunges(void); /* rounds that lost live entries, and clears: a reader's miss after one may be an eviction */

#endif
