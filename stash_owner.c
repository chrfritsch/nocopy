/* stash arena: owner ids of lock holders, worker slots and leases. */

#include "stash_arena_int.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
# include <sys/sysctl.h>
#else
# include <fcntl.h>
#endif

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

uint64_t stash_owner_self(void)
{
	int pid = getpid();
	return (uint64_t) (uint32_t) pid | (uint64_t) proc_start(pid) << 32;
}

bool stash_owner_dead(uint64_t id)
{
	int pid = (int) (uint32_t) id;
	if (kill(pid, 0) == -1 && errno == ESRCH) {
		return true;
	}
	return proc_start(pid) != (uint32_t) (id >> 32);
}
