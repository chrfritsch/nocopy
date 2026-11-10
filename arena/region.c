/* nocopy arena: the shared-memory region: create, attach, liveness, removal of unused objects. */

#include "region.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define AR_MAGIC 0x6e6f636f70790001ULL

/* The object, open for the process's life with a shared flock on it (inherited by forks):
 * an exclusive flock succeeds only while no process maps the object. */
static int named_fd = -1;
static char named_name[64];
static size_t mapped_size; /* not the header's: workers may write it */
static struct stat named_st; /* the object's identity, also after ar_region_unhold */

static bool same_file(const struct stat *a, const struct stat *b) { return a->st_dev == b->st_dev && a->st_ino == b->st_ino; }

/* Ours alone: another user's object would hand us its pointers (root ignores the mode). */
static bool owned(int fd)
{
	struct stat st;
	return fstat(fd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 077) == 0;
}

static bool same_object(const char *name, int fd)
{
	struct stat a, b;
	int cur = shm_open(name, O_RDONLY, 0);
	bool same = cur >= 0 && fstat(fd, &a) == 0 && fstat(cur, &b) == 0 && same_file(&a, &b);
	if (cur >= 0) {
		close(cur);
	}
	return same;
}

/* Unlinks name if no process uses fd's object and name still refers to it. false: in use. */
static bool remove_unused(const char *name, int fd)
{
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		return false;
	}
	if (same_object(name, fd)) {
		shm_unlink(name);
	}
	flock(fd, LOCK_UN);
	return true;
}

/* Marks fd's object as used by this process. false: name was replaced meanwhile, retry. */
static bool hold(const char *name, int fd)
{
	while (flock(fd, LOCK_SH) != 0 && errno == EINTR) {}
	return same_object(name, fd);
}

/* fd mapped exactly at base, or NULL. */
static void *map_at(int fd, size_t size, uintptr_t base)
{
	int flags = MAP_SHARED;
#ifdef MAP_FIXED_NOREPLACE
	flags |= MAP_FIXED_NOREPLACE; /* older kernels treat it as a hint: checked below */
#endif
	void *p = mmap((void *) base, size, PROT_READ | PROT_WRITE, flags, fd, 0);
	if (p == MAP_FAILED) {
		return NULL;
	}
	if ((uintptr_t) p != base) {
		munmap(p, size);
		return NULL;
	}
	return p;
}

/* Far from PIE binaries, heaps, mmap bases and Go's arenas (FrankenPHP); the second fits 39-bit address spaces. */
static const uintptr_t bases[] = {(uintptr_t) 0x200000000000ULL, (uintptr_t) 0x1000000000ULL,
#ifdef __SANITIZE_ADDRESS__
	(uintptr_t) 0x300000000000ULL, /* both above are ASan shadow on aarch64 */
#endif
};

typedef enum { OPEN_OK, OPEN_RETRY, OPEN_FAIL } open_result;

typedef struct {
	uint64_t abi;
	size_t size;
	void (*init)(void *mem, void *arg);
	void *arg;
	void *mem;
	const char *err;
} opening;

static open_result open_fail(int fd, opening *o, const char *why)
{
	close(fd);
	o->err = why;
	return OPEN_FAIL;
}

/* fd: just created (O_EXCL), empty. */
static open_result create_new(const char *name, int fd, opening *o)
{
	if (!hold(name, fd)) {
		close(fd);
		return OPEN_RETRY; /* removed as unused before we held it */
	}
#ifdef __linux__
	/* reserve every page now: a sparse tmpfs file over /dev/shm's limit (Docker: 64M) would SIGBUS on first touch */
	int r = posix_fallocate(fd, 0, (off_t) o->size);
	o->err = r == ENOSPC ? "not enough room in /dev/shm (Docker: --shm-size)" : r ? "posix_fallocate failed" : NULL;
#else
	o->err = ftruncate(fd, (off_t) o->size) ? "ftruncate failed" : NULL;
#endif
	void *p = NULL;
	for (size_t i = 0; !o->err && !p && i < sizeof bases / sizeof bases[0]; i++) {
		p = map_at(fd, o->size, bases[i]);
	}
	if (!p) {
		o->err = o->err ? o->err : "no free address for it";
		shm_unlink(name);
		close(fd);
		return OPEN_FAIL;
	}
	ar_region_hdr *h = p;
	h->abi = o->abi;
	h->base = (uintptr_t) p;
	h->size = o->size;
	o->init(p, o->arg);
	mapped_size = o->size;
	__atomic_store_n(&h->magic, AR_MAGIC, __ATOMIC_RELEASE);
	named_fd = fd;
	o->mem = p;
	return OPEN_OK;
}

/* fd: an existing object, maybe still being initialized by its creator. */
static open_result attach(const char *name, int fd, opening *o)
{
	ar_region_hdr hdr = {0};
	if (!owned(fd)) {
		return open_fail(fd, o, "it is owned by another user or open to others");
	}
	for (int waited = 0;; waited++) {
		struct stat st;
		if (fstat(fd, &st) == 0 && (size_t) st.st_size >= sizeof hdr) {
			ar_region_hdr *q = mmap(NULL, sizeof hdr, PROT_READ, MAP_SHARED, fd, 0);
			if (q != MAP_FAILED) {
				hdr.magic = __atomic_load_n(&q->magic, __ATOMIC_ACQUIRE);
				hdr.abi = q->abi;
				hdr.base = q->base;
				hdr.size = q->size;
				munmap(q, sizeof hdr);
			}
		}
		if (hdr.magic == AR_MAGIC) {
			break;
		}
		if (remove_unused(name, fd)) {
			close(fd);
			return OPEN_RETRY; /* its creator died before it was set up */
		}
		if (waited == 2000) {
			return open_fail(fd, o, "its creator has not finished setting it up for 2 s");
		}
		usleep(1000);
	}
	if (hdr.abi != o->abi) {
		return open_fail(fd, o, "it belongs to another build");
	}
	if (!hold(name, fd)) {
		close(fd);
		return OPEN_RETRY;
	}
	void *p = map_at(fd, hdr.size, hdr.base);
	if (!p) {
		return open_fail(fd, o, "its address is taken in this process");
	}
	named_fd = fd;
	mapped_size = hdr.size;
	o->mem = p;
	return OPEN_OK;
}

#ifdef __linux__
#include <dirent.h>

/* Other builds' regions under the same base (base.<8 hex digits>): unused ones (left by an upgrade) are removed, one still in use is reported. */
static void collect(const char *base, nocopy_named *out)
{
	DIR *d = opendir("/dev/shm");
	if (!d) {
		return;
	}
	size_t bl = strlen(base + 1);
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *n = de->d_name, *x = n + bl + 1;
		if (strncmp(n, base + 1, bl) != 0 /* NOLINT(bugprone-not-null-terminated-result): stops at NUL */ || n[bl] != '.' || strlen(x) != 8 || strspn(x, "0123456789abcdef") != 8 || strcmp(n, out->name + 1) == 0) {
			continue;
		}
		char other[sizeof out->other];
		snprintf(other, sizeof other, "/%.62s", n); /* n is at most 49 chars (checked above) */
		int fd = shm_open(other, O_RDWR, 0);
		if (fd < 0 || !owned(fd)) {
			if (fd >= 0) {
				close(fd);
			}
			continue; /* another user's: not ours to judge */
		}
		if (!remove_unused(other, fd) && !out->other[0]) {
			memcpy(out->other, other, sizeof other);
		}
		close(fd);
	}
	closedir(d);
}
#endif

static nocopy_err failed(const char *msg) { return (nocopy_err) {.code = NOCOPY_ERR_FAILED, .msg = msg}; }

nocopy_err ar_region_open(const char *base, uint64_t abi, size_t size, void (*init)(void *mem, void *arg), void *arg, nocopy_named *out, void **mem)
{
	memset(out, 0, sizeof *out);
	*mem = NULL;
	if (base[0] != '/' || strchr(base + 1, '/') || !base[1] || strlen(base) > 40) {
		return failed("not a name like /nocopy (one leading slash, at most 40 characters)");
	}
	snprintf(out->name, sizeof out->name, "%s.%08x", base, (uint32_t) (abi ^ abi >> 32));
	opening o = {.abi = abi, .size = size, .init = init, .arg = arg};
	for (int attempt = 0; attempt < 8; attempt++) {
		int fd = shm_open(out->name, O_RDWR | O_CREAT | O_EXCL, 0600);
		open_result r;
		if (fd >= 0) {
			r = create_new(out->name, fd, &o);
		} else if (errno != EEXIST) {
			return failed(errno == EACCES ? "no permission to create it" : "shm_open failed");
		} else if ((fd = shm_open(out->name, O_RDWR, 0)) < 0) {
			if (errno == ENOENT) {
				continue; /* removed meanwhile */
			}
			return failed(errno == EACCES ? "no permission to open it (another user created it)" : "shm_open failed");
		} else {
			r = attach(out->name, fd, &o);
		}
		if (r == OPEN_FAIL) {
			return failed(o.err);
		}
		if (r == OPEN_OK) {
			memcpy(named_name, out->name, sizeof named_name);
			(void) fstat(named_fd, &named_st);
#ifdef __linux__
			collect(base, out);
#endif
			*mem = o.mem;
			return (nocopy_err) {.code = NOCOPY_ERR_NONE};
		}
	}
	return failed("it was replaced again and again while opening it");
}

void ar_region_unhold(void)
{
	if (named_fd >= 0) {
		close(named_fd); /* the flock stays with the other processes sharing the open file */
		named_fd = -1;
	}
}

bool ar_region_current(void)
{
	struct stat b;
#ifdef __linux__
	char path[80];
	snprintf(path, sizeof path, "/dev/shm%s", named_name); /* stat needs no permission on the object: fpm workers run as another user */
	return stat(path, &b) == 0 && same_file(&b, &named_st);
#else
	int cur = shm_open(named_name, O_RDONLY, 0);
	bool same = cur >= 0 && fstat(cur, &b) == 0 && same_file(&b, &named_st);
	if (cur >= 0) {
		close(cur);
	} else if (errno == EACCES) {
		return true; /* it exists, not ours to open: assume it is still ours */
	}
	return same;
#endif
}

int ar_region_reopen(void)
{
	struct stat st;
	int fd = shm_open(named_name, O_RDWR, 0);
#ifdef __linux__
	if (fd < 0 && named_fd >= 0) { /* its name was removed: through our descriptor */
		char path[32];
		snprintf(path, sizeof path, "/proc/self/fd/%d", named_fd);
		fd = open(path, O_RDWR | O_CLOEXEC);
	}
#endif
	if (fd >= 0 && (fstat(fd, &st) != 0 || !same_file(&st, &named_st))) {
		close(fd);
		fd = -1;
	}
	return fd;
}

void ar_region_close(void *mem)
{
	if (mem) {
		munmap(mem, mapped_size);
	}
	if (named_fd >= 0) {
		close(named_fd); /* drops our shared flock */
		named_fd = -1;
	}
}
