/* Threaded stress test of the arena without PHP: build with tests/c/run.sh (plain -O2 and ThreadSanitizer).
 * Each thread writes, deletes and reads its own keys against a model, reads every other thread's keys,
 * and holds some values pinned across later operations: a value read must always be intact. */
#include "../../nocopy_arena.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define KEYS 600     /* per thread */
#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))

static int nthreads = 8, seconds = 3;
static volatile int stop;
static uint64_t failures;

typedef struct { uint32_t ver, len; } payload; /* after the key; then len bytes of a pattern of (key, ver) */

void nocopy_rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) e, (void) from; /* the payload holds no pointers */
}

static uint64_t hash(const char *k, size_t n)
{
	uint64_t h = 5381;
	while (n--) {
		h = h * 33 + (unsigned char) *k++;
	}
	return h | 1;
}

static uint8_t pat(uint64_t h, uint32_t ver, uint32_t i) { return (uint8_t) (h ^ ver * 31 ^ i * 7); }

static payload *pl(nocopy_entry *e) { return (payload *) ((char *) e + HDR + NOCOPY_ALIGN(e->klen + 1)); }

/* Checks a found entry; returns its ver or -1 if corrupt. */
static long check(nocopy_entry *e, uint64_t h, const char *k, size_t klen)
{
	payload *p = pl(e);
	const uint8_t *d = (const uint8_t *) (p + 1);
	if (e->h != h || e->klen != klen || memcmp(nocopy_key(e), k, klen)) {
		return -1;
	}
	if ((char *) (d + p->len) > (char *) e + e->size) {
		return -1;
	}
	for (uint32_t i = 0; i < p->len; i++) {
		if (d[i] != pat(h, p->ver, i)) {
			return -1;
		}
	}
	return p->ver;
}

static void fail(const char *what, const char *k)
{
	__atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
	fprintf(stderr, "FAIL %s %s\n", what, k);
}

static bool put(nocopy_ctx *c, const char *k, uint32_t ver, uint32_t len, time_t ttl)
{
	size_t klen = strlen(k), total = HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(sizeof(payload) + len);
	uint64_t h = hash(k, klen);
	nocopy_put p;
	nocopy_entry *e = nocopy_put_begin(&p, c, h, (uint32_t) klen, HDR, total, ttl);
	if (!e) {
		return false;
	}
	memcpy((char *) e + HDR, k, klen + 1);
	payload *pp = pl(e);
	pp->ver = ver;
	pp->len = len;
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *) (pp + 1))[i] = pat(h, ver, i);
	}
	return nocopy_put_commit(&p, false) == PUT_OK;
}

static void del(const char *k)
{
	uint64_t h = hash(k, strlen(k));
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	nocopy_remove(sh, h, k, (uint32_t) strlen(k));
	nocopy_unlock(sh);
}

typedef struct { nocopy_entry *e; uint64_t h; char k[32]; long ver; } held;

static void *worker(void *arg)
{
	int me = (int) (intptr_t) arg;
	unsigned seed = (unsigned) me * 7919 + 1;
	nocopy_ctx c = {.slot = -1};
	if (!nocopy_claim(&c)) {
		fail("claim", "");
		return NULL;
	}
	static const uint32_t sizes[] = {8, 100, 1000, 3000, 20000, 200000};
	long *model = calloc(KEYS, sizeof(long));   /* expected ver, -1 absent */
	uint64_t *at = calloc(KEYS, sizeof(uint64_t)); /* expunges when stored */
	for (int i = 0; i < KEYS; i++) {
		model[i] = -1;
	}
	held hold[16];
	int nh = 0;
	char k[32];
	uint32_t ver = 0;
	while (!__atomic_load_n(&stop, __ATOMIC_RELAXED)) {
		int i = rand_r(&seed) % KEYS, r = rand_r(&seed) % 100;
		snprintf(k, sizeof k, "t%d_%d", me, i);
		if (r < 35) {
			uint64_t ex = __atomic_load_n(&nocopy_a->expunges, __ATOMIC_RELAXED);
			uint32_t len = sizes[rand_r(&seed) % (r < 2 ? 6 : 5)];
			bool ttl = rand_r(&seed) % 8 == 0;
			if (put(&c, k, ++ver, len, ttl ? 1 + rand_r(&seed) % 2 : 0)) {
				model[i] = ttl ? -2 : (long) ver; /* -2: may have expired */
				at[i] = ex;
			} else {
				del(k);
				model[i] = -1;
			}
		} else if (r < 50) {
			del(k);
			model[i] = -1;
		} else {
			bool other = r < 75;
			if (other) { /* someone else's key: only integrity can be checked */
				snprintf(k, sizeof k, "t%d_%d", rand_r(&seed) % nthreads, i);
			}
			size_t klen = strlen(k);
			uint64_t h = hash(k, klen);
			nocopy_borrow_begin(&c);
			nocopy_entry *e = nocopy_find(&c, h, k, (uint32_t) klen);
			long got = e ? check(e, h, k, klen) : -1;
			if (e && got < 0) {
				fail("corrupt read", k);
			}
			if (!other && model[i] != -2) {
				uint64_t ex = __atomic_load_n(&nocopy_a->expunges, __ATOMIC_RELAXED);
				if (e ? got != model[i] : model[i] >= 0 && ex == at[i]) {
					fail(e ? "wrong version" : "lost key", k);
				}
			}
			if (e && nh < 16 && rand_r(&seed) % 4 == 0) { /* hold it like a zero-copy value */
				nocopy_keep(&c, e);
				hold[nh++] = (held) {e, h, "", got};
				strcpy(hold[nh - 1].k, k);
			}
			nocopy_borrow_end(&c);
		}
		if (nh && rand_r(&seed) % 50 == 0) { /* end of "request": held values must be untouched, then let go */
			for (int j = 0; j < nh; j++) {
				if (check(hold[j].e, hold[j].h, hold[j].k, strlen(hold[j].k)) != hold[j].ver) {
					fail("held value changed", hold[j].k);
				}
			}
			nh = 0;
			nocopy_keep_release(&c);
		}
	}
	nocopy_release(&c);
	free(model);
	free(at);
	return NULL;
}

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? (size_t) atoi(argv[1]) : 8;
	nthreads = argc > 2 ? atoi(argv[2]) : 8;
	seconds = argc > 3 ? atoi(argv[3]) : 3;
	for (uint32_t n = 0; n <= 40; n++) { /* nocopy_key_eq: equal, and a difference at every position */
		char a[41], b[41];
		memset(a, 'k', n); memcpy(b, a, n);
		assert(nocopy_key_eq(a, b, n));
		for (uint32_t i = 0; i < n; i++) {
			b[i] ^= 1;
			assert(!nocopy_key_eq(a, b, n));
			b[i] ^= 1;
		}
	}
	char base[32];
	nocopy_named nm;
	snprintf(base, sizeof base, "/nocopy-stress-%d", (int) getpid());
	const char *err = nocopy_arena_open(base, mb << 20, 0, 1, &(size_t){0}, &nm);
	shm_unlink(nm.name); /* this run's only: the mapping stays */
	if (err) {
		fprintf(stderr, "%s\n", err);
		return 2;
	}
	pthread_t t[64];
	for (int i = 0; i < nthreads; i++) {
		pthread_create(&t[i], NULL, worker, (void *) (intptr_t) i);
	}
	sleep((unsigned) seconds);
	__atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
	for (int i = 0; i < nthreads; i++) {
		pthread_join(t[i], NULL);
	}
	nocopy_stats_t s;
	nocopy_stats(&s);
	printf("%zu MB, %d threads, %ds: %llu entries, %u shards, %llu expunges, %llu MB moved, failures %llu\n", mb, nthreads, seconds,
		(unsigned long long) s.entries, s.shards, (unsigned long long) s.expunges, (unsigned long long) (s.evacuated >> 20),
		(unsigned long long) failures);
	return failures ? 1 : 0;
}
