/* nocopy arena: owner ids, locks that survive a dead holder, the worker registry. */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE /* F_OFD_* */
#endif
#include "proc.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef __APPLE__
# include <sys/sysctl.h>
#endif

uint64_t ar_self;
ar_procs *ar_proc;
#ifdef NOCOPY_HOOKS
void (*ar_hook)(const char *point);
const void *ar_hook_on;
#endif

static time_t checked[NOCOPY_MAX_WORKERS]; /* last owner lookup per slot (ar_worker_dead) */
static uint64_t gone[NOCOPY_MAX_WORKERS];  /* the owner that lookup found dead: an owner id never comes back */

/* ---- owner ids ---- */

static int live_fd = -1; /* ours alone (not a parent's): its OFD lock on our process slot's byte is our liveness */

#ifdef __linux__
static bool slot_lock(int fd, uint32_t s, int cmd, struct flock *fl)
{
	*fl = (struct flock) {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = s, .l_len = 1};
	return fcntl(fd, cmd, fl) == 0;
}

bool ar_proc_attach(int fd)
{
	if (live_fd >= 0) {
		close(live_fd); /* a forked child's copy of its parent's: the parent's lock stays */
	}
	live_fd = fd;
	ar_self = 0;
	uint32_t start = (uint32_t) getpid() % NOCOPY_MAX_WORKERS;
	for (uint32_t k = 0; fd >= 0 && k < NOCOPY_MAX_WORKERS; k++) {
		uint32_t s = (start + k) % NOCOPY_MAX_WORKERS;
		struct flock fl;
		if (slot_lock(fd, s, F_OFD_SETLK, &fl)) {
			uint32_t g = __atomic_add_fetch(&ar_proc->pgen[s], 1, __ATOMIC_SEQ_CST); /* after the lock: see ar_owner_dead */
			ar_self = (uint64_t) (s + 1) | (uint64_t) g << 32;
			return true;
		}
		if (errno != EAGAIN && errno != EACCES) {
			break; /* no OFD locks here */
		}
	}
	ar_proc_detach();
	return false;
}

bool ar_owner_dead(uint64_t id)
{
	uint32_t s = (uint32_t) id - 1;
	struct flock fl;
	if (id == ar_self) {
		return false; /* our own lock does not conflict with us */
	}
	if (s >= NOCOPY_MAX_WORKERS) {
		return true;
	}
	if (!slot_lock(live_fd, s, F_OFD_GETLK, &fl)) {
		return false; /* cannot tell: alive */
	}
	/* gen after the query: a newcomer holding the byte has bumped it, or is about to (alive until the next look) */
	return fl.l_type == F_UNLCK || __atomic_load_n(&ar_proc->pgen[s], __ATOMIC_SEQ_CST) != (uint32_t) (id >> 32);
}
#else
/* Start time of pid (any stable 32-bit stamp), 0 if it is gone or a zombie. */
static uint32_t proc_start(int pid)
{
#ifdef __APPLE__
	struct kinfo_proc kp;
	size_t len = sizeof kp;
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
	if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof kp || kp.kp_proc.p_stat == SZOMB) {
		return 0;
	}
	return (uint32_t) (kp.kp_proc.p_starttime.tv_sec * 1000003 + kp.kp_proc.p_starttime.tv_usec) | 1;
#else
	(void) pid;
	return 0;
#endif
}

bool ar_proc_attach(int fd)
{
	if (fd >= 0) {
		close(fd);
	}
	int pid = getpid();
	ar_self = (uint64_t) (uint32_t) pid | (uint64_t) proc_start(pid) << 32;
	return true;
}

bool ar_owner_dead(uint64_t id)
{
	int pid = (int) (uint32_t) id;
	if (kill(pid, 0) == -1 && errno == ESRCH) {
		return true;
	}
	return proc_start(pid) != (uint32_t) (id >> 32);
}
#endif

int nocopy_live_fd(void) { return live_fd; }

void ar_proc_detach(void)
{
	if (live_fd >= 0) {
		close(live_fd);
		live_fd = -1;
	}
	ar_self = 0;
}

/* ---- locks ---- */

/* Robust pthread mutexes were measured 15% slower uncontended and 2-8x slower under contention (futex sleeps): this keeps the spin. */
ar_lock_result ar_lock_wait(ar_lock *l, uint64_t *dead)
{
	for (unsigned spins = 0;;) {
		uint64_t h = __atomic_load_n(&l->holder, __ATOMIC_RELAXED);
		if (!h) {
			if (__atomic_compare_exchange_n(&l->holder, &h, ar_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				return AR_LOCKED;
			}
		} else if (++spins < 128) {
			AR_HOOK_ON("lock.wait", l);
			ar_cpu_relax();
		} else {
			AR_HOOK_ON("lock.wait", l);
			sched_yield();
			if (!(spins & 1023) && h != ar_self && ar_owner_dead(h)
			 && __atomic_compare_exchange_n(&l->holder, &h, ar_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				__atomic_add_fetch(&ar_proc->lock_steals, 1, __ATOMIC_RELAXED);
				*dead = h;
				return AR_STOLEN;
			}
		}
	}
}

ar_try_result ar_lock_try(ar_lock *l, time_t *checked_at, time_t now)
{
	uint64_t z = 0;
	if (__atomic_compare_exchange_n(&l->holder, &z, ar_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return AR_TRY_LOCKED;
	}
	if (z == ar_self || *checked_at == now) { /* ar_self: another thread of this process */
		return AR_TRY_BUSY;
	}
	*checked_at = now;
	if (!ar_owner_dead(z) || !__atomic_compare_exchange_n(&l->holder, &z, ar_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return AR_TRY_BUSY;
	}
	__atomic_add_fetch(&ar_proc->lock_steals, 1, __ATOMIC_RELAXED);
	return AR_TRY_STOLEN;
}

/* ---- workers ---- */

void ar_proc_adopt(ar_procs *p) { ar_proc = p; }

int ar_worker_claim(void)
{
	if (!ar_self) {
		return -1;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < NOCOPY_MAX_WORKERS; i++) {
			uint64_t o = __atomic_load_n(&ar_proc->w[i].owner, __ATOMIC_RELAXED);
			if (pass ? !o || !ar_owner_dead(o) : o != 0) {
				continue;
			}
			if (__atomic_compare_exchange_n(&ar_proc->w[i].owner, &o, ar_self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				uint32_t cur = __atomic_load_n(&ar_proc->nworkers, __ATOMIC_RELAXED);
				while (cur < (uint32_t) i + 1
				 && !__atomic_compare_exchange_n(&ar_proc->nworkers, &cur, i + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
				return i;
			}
		}
	}
	return -1;
}

void ar_worker_release(int slot) { __atomic_store_n(&ar_proc->w[slot].owner, 0, __ATOMIC_SEQ_CST); }

bool ar_worker_dead(uint32_t i, time_t now)
{
	uint64_t o = __atomic_load_n(&ar_proc->w[i].owner, __ATOMIC_SEQ_CST); /* after the caller read the slot's state: a new owner's is never taken for the dead one's */
	if (!o || o == ar_self) {
		return false;
	}
	if (__atomic_load_n(&gone[i], __ATOMIC_RELAXED) == o) {
		return true;
	}
	if (__atomic_exchange_n(&checked[i], now, __ATOMIC_RELAXED) == now || !ar_owner_dead(o)) {
		return false;
	}
	__atomic_store_n(&gone[i], o, __ATOMIC_RELAXED);
	return true;
}

bool ar_worker_live(uint32_t i)
{
	uint64_t o = __atomic_load_n(&ar_proc->w[i].owner, __ATOMIC_RELAXED);
	return o && (o == ar_self || !ar_owner_dead(o));
}
