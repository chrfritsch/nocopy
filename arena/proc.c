/* nocopy arena: owner ids, locks that survive a dead holder, the worker registry. */

#include "proc.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
# include <sys/sysctl.h>
#else
# include <fcntl.h>
#endif

uint64_t ar_self;
ar_procs *ar_proc;

static time_t checked[NOCOPY_MAX_WORKERS]; /* last owner lookup per slot (ar_worker_dead) */

/* ---- owner ids ---- */

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
	char path[32], buf[1024];
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) {
		return 0;
	}
	buf[n] = 0;
	char *p = strrchr(buf, ')'); /* "pid (comm) S ...": comm may hold anything; starttime is field 22 */
	if (!p || p[1] != ' ' || p[2] == 'Z') {
		return 0;
	}
	for (int f = 2; f < 22 && p; f++) {
		p = strchr(p + 1, ' ');
	}
	return p ? (uint32_t) strtoull(p + 1, NULL, 10) | 1 : 0;
#endif
}

uint64_t ar_owner_self(void)
{
	int pid = getpid();
	return (uint64_t) (uint32_t) pid | (uint64_t) proc_start(pid) << 32;
}

bool ar_owner_dead(uint64_t id)
{
	int pid = (int) (uint32_t) id;
	if (kill(pid, 0) == -1 && errno == ESRCH) {
		return true;
	}
	return proc_start(pid) != (uint32_t) (id >> 32);
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
			ar_cpu_relax();
		} else {
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

void ar_proc_adopt(ar_procs *p)
{
	ar_proc = p;
	ar_self = ar_owner_self();
}

void ar_proc_after_fork(void) { ar_self = ar_owner_self(); }

int ar_worker_claim(void)
{
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
	uint64_t o = __atomic_load_n(&ar_proc->w[i].owner, __ATOMIC_RELAXED);
	return o && o != ar_self && __atomic_exchange_n(&checked[i], now, __ATOMIC_RELAXED) != now && ar_owner_dead(o);
}

bool ar_worker_live(uint32_t i)
{
	uint64_t o = __atomic_load_n(&ar_proc->w[i].owner, __ATOMIC_RELAXED);
	return o && (o == ar_self || !ar_owner_dead(o));
}
