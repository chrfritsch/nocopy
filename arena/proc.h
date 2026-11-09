/* nocopy arena: processes. Owner ids, locks that survive a dead holder, and the worker registry. */

#ifndef NOCOPY_ARENA_PROC_H
#define NOCOPY_ARENA_PROC_H

#include "base.h"

/* ---- owner ids: pid | process start time << 32, so a recycled pid does not look like its dead predecessor ---- */

AR_HIDDEN extern uint64_t ar_self; /* this process's; threads share it */
uint64_t ar_owner_self(void);
bool ar_owner_dead(uint64_t id); /* gone, a zombie, or its pid reused; reads /proc: call it with no lock held */

/* ---- locks: test-and-test-and-set holding the owner id; not reentrant ---- */

typedef struct {
	uint64_t holder; /* owner id, 0 free; its user puts it on a cache line of its own */
} ar_lock;

/* STOLEN: taken over from a dead holder, whose half-done work the caller must repair. */
typedef enum { AR_LOCKED, AR_STOLEN } ar_lock_result;

/* Slow path: spins, yields, and takes the lock over from a dead holder (*dead: its id). A holder's own process never looks dead. */
ar_lock_result ar_lock_wait(ar_lock *l, uint64_t *dead);

static inline ar_lock_result ar_lock_acquire(ar_lock *l, uint64_t *dead)
{
	uint64_t z = 0;
	if (__atomic_compare_exchange_n(&l->holder, &z, ar_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return AR_LOCKED;
	}
	return ar_lock_wait(l, dead);
}

static inline void ar_lock_release(ar_lock *l) { __atomic_store_n(&l->holder, 0, __ATOMIC_RELEASE); }

typedef enum { AR_TRY_LOCKED, AR_TRY_STOLEN, AR_TRY_BUSY } ar_try_result;
/* One attempt, for work that can be skipped. A holder found dead (looked at once a second: *checked) is taken over. */
ar_try_result ar_lock_try(ar_lock *l, time_t *checked, time_t now);

/* ---- workers: a slot per process (NTS) or thread (ZTS) that reads or writes the arena ---- */

/* One cache line per worker, each field written by one module only: what it needs from a worker that may die. */
typedef struct {
	uint64_t owner;       /* proc: owner id, 0 free */
	uint64_t hits, misses; /* the API's lookup counters (nocopy_count): single writer, no RMW */
	uint32_t wip;         /* shard: offset/8 of the entry it fills outside the shard lock, 0 none; changes under that shard's lock */
	uint32_t agen;        /* atoms: the generation its write in progress may reference, 0 none */
} __attribute__((aligned(64))) ar_worker;

typedef struct {
	uint32_t nworkers;    /* high-water mark of claimed slots */
	uint32_t lock_steals; /* locks taken over from dead holders */
	ar_worker w[NOCOPY_MAX_WORKERS];
} ar_procs;

AR_HIDDEN extern ar_procs *ar_proc;

void ar_proc_adopt(ar_procs *p); /* this process's view, and its owner id */
void ar_proc_after_fork(void);   /* a child gets its own owner id */

/* A free slot, or one whose owner died: -1 if none. The caller resets what each module keeps per slot. */
int ar_worker_claim(void);
void ar_worker_release(int slot);
static inline uint32_t ar_worker_high(void) { return __atomic_load_n(&ar_proc->nworkers, __ATOMIC_ACQUIRE); }
static inline ar_worker *ar_worker_at(uint32_t slot) { return &ar_proc->w[slot]; }
/* Slot i is held by a process that died. Looked up at most once a second per slot; false meanwhile, and for our own process. */
bool ar_worker_dead(uint32_t i, time_t now);
/* Slot i is held by a live process (ours included). Reads /proc: call it with no lock held, or rarely. */
bool ar_worker_live(uint32_t i);

#endif
