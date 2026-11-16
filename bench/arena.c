/* The arena alone, without PHP, over a keyspace too large for the CPU caches: where slot and entry lines miss, and locks are held meanwhile.
 *   cc -O2 -Iarena -o /tmp/arena-bench bench/arena.c arena/?*.c -lpthread
 *   /tmp/arena-bench <mb> <keys> <threads> <ms per phase> <rounds> <batch> ops...
 * ops: none (the harness), get, multi (batches, key by key), multiwarm (batches behind nocopy_warm; multiwarm2: behind nocopy_warm_slot too), put, inc, del (delete + put).
 * The ops alternate for <rounds>; prints each one's best ns per op and thread. Compare two trees with interleaved runs (taskset -c N). */
#include "nocopy_arena.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
static uint32_t nkeys;
static int nthreads, phase_ms, rounds, batch = 16;
static const char *op;
static volatile int stop;
static uint64_t total[64];
static uint64_t *hashes; /* per key: PHP caches a string's hash too */

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from) { (void) e; (void) from; return 0; }

static uint64_t hash(const char *k, size_t n)
{
	uint64_t h = 5381;
	while (n--) {
		h = h * 33 + (unsigned char) *k++;
	}
	return h | 1;
}

static uint32_t keyname(char *k, uint32_t i)
{
	memcpy(k, "bench:key:", 10);
	for (int d = 0; d < 8; d++) {
		k[10 + d] = "0123456789abcdef"[(i >> (28 - 4 * d)) & 15];
	}
	k[18] = 0;
	return 18;
}

static bool put(nocopy_ctx *c, uint32_t i, uint64_t v)
{
	char k[32];
	uint32_t klen = keyname(k, i);
	nocopy_put p;
	nocopy_put_req q = {.h = hashes[i], .key = k, .klen = klen, .koff = HDR, .size = HDR + NOCOPY_ALIGN(klen + 1) + 16};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q);
	if (!e) {
		return false;
	}
	memcpy((char *) e + HDR + NOCOPY_ALIGN(klen + 1), &v, 8);
	return nocopy_put_commit(&p, false) == NOCOPY_PUT_OK;
}

static int bump(nocopy_entry *e, void *arg)
{
	(void) arg;
	__atomic_add_fetch((uint64_t *) ((char *) e + HDR + NOCOPY_ALIGN(nocopy_entry_klen(e) + 1)), 1, __ATOMIC_RELAXED);
	return 1;
}

static inline uint32_t rnd(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	*s = x;
	return (uint32_t) (x >> 20);
}

static void *worker(void *arg)
{
	int id = (int) (intptr_t) arg;
	nocopy_ctx c;
	nocopy_ctx_init(&c);
	if (!nocopy_claim(&c)) {
		fprintf(stderr, "claim\n");
		exit(2);
	}
	uint64_t s = 0x9E3779B97F4A7C15ULL * (uint64_t) (id + 1), n = 0, sum = 0;
	char k[64][32];
	uint32_t kl[64], ki[64];
	while (!__atomic_load_n(&stop, __ATOMIC_RELAXED)) {
		if (!strcmp(op, "get")) {
			for (int r = 0; r < 64; r++) {
				uint32_t i = rnd(&s) % nkeys, klen = keyname(k[0], i);
				nocopy_borrow_begin(&c);
				nocopy_entry *e = nocopy_find(&c, hashes[i], k[0], klen);
				sum += e ? nocopy_entry_klen(e) : 0;
				nocopy_borrow_end(&c);
			}
			n += 64;
		} else if (!strncmp(op, "multi", 5)) {
			for (int b = 0; b < batch; b++) {
				ki[b] = rnd(&s) % nkeys;
				kl[b] = keyname(k[b], ki[b]);
			}
			nocopy_borrow_begin(&c);
			if (op[5]) {
				for (int b = 0; b < batch && op[9]; b++) { /* multiwarm2: the slots first, in a stage of their own */
					nocopy_warm_slot(hashes[ki[b]]);
				}
				for (int b = 0; b < batch; b++) {
					nocopy_warm(hashes[ki[b]]);
				}
			}
			for (int b = 0; b < batch; b++) {
				nocopy_entry *e = nocopy_find(&c, hashes[ki[b]], k[b], kl[b]);
				sum += e ? nocopy_entry_klen(e) : 0;
			}
			nocopy_borrow_end(&c);
			n += (uint64_t) batch;
		} else if (!strcmp(op, "put")) {
			for (int r = 0; r < 64; r++) {
				put(&c, rnd(&s) % nkeys, n);
			}
			n += 64;
		} else if (!strcmp(op, "inc")) {
			for (int r = 0; r < 64; r++) {
				uint32_t i = rnd(&s) % nkeys, klen = keyname(k[0], i);
				nocopy_update(hashes[i], k[0], klen, bump, NULL);
			}
			n += 64;
		} else if (!strcmp(op, "none")) { /* the harness alone */
			for (int r = 0; r < 64; r++) {
				uint32_t i = rnd(&s) % nkeys, klen = keyname(k[0], i);
				sum += hashes[i] + klen;
			}
			n += 64;
		} else { /* del: delete, then put back */
			for (int r = 0; r < 32; r++) {
				uint32_t i = rnd(&s) % nkeys, klen = keyname(k[0], i);
				nocopy_delete(hashes[i], k[0], klen);
				put(&c, i, n);
			}
			n += 64;
		}
	}
	total[id] = n;
	nocopy_release(&c);
	return (void *) (uintptr_t) sum;
}

int main(int argc, char **argv)
{
	if (argc < 7) {
		fprintf(stderr, "arena-bench <mb> <keys> <threads> <ms per phase> <rounds> <batch> ops... (none get multi multiwarm multiwarm2 put inc del)\n");
		return 2;
	}
	size_t mb = (size_t) atoi(argv[1]);
	nkeys = (uint32_t) atoi(argv[2]);
	nthreads = atoi(argv[3]);
	phase_ms = atoi(argv[4]);
	rounds = atoi(argv[5]);
	batch = atoi(argv[6]);
	char base[32];
	nocopy_named nm;
	snprintf(base, sizeof base, "/nocopy-bench-%d", (int) getpid());
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = mb << 20, .hint = nkeys, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name);
	if (err.code != NOCOPY_ERR_NONE) {
		fprintf(stderr, "%s\n", err.msg);
		return 2;
	}
	nocopy_ctx mc;
	nocopy_ctx_init(&mc);
	nocopy_claim(&mc);
	hashes = malloc(nkeys * sizeof *hashes);
	for (uint32_t i = 0; i < nkeys; i++) {
		char k[32];
		hashes[i] = hash(k, keyname(k, i));
	}
	for (uint32_t i = 0; i < nkeys; i++) { /* in scattered order: neighbours in the log are not neighbours in the key space */
		put(&mc, (uint32_t) (((uint64_t) i * 2654435761u) % nkeys), 0);
	}
	nocopy_request_end(&mc);
	int nops = argc - 7;
	double best[32];
	for (int o = 0; o < nops; o++) {
		best[o] = 1e18;
	}
	for (int r = 0; r < rounds; r++) {
		for (int o = 0; o < nops; o++) {
			op = argv[7 + o];
			stop = 0;
			pthread_t t[64];
			for (int i = 0; i < nthreads; i++) {
				pthread_create(&t[i], NULL, worker, (void *) (intptr_t) i);
			}
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			usleep((unsigned) phase_ms * 1000);
			__atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
			uint64_t sum = 0;
			for (int i = 0; i < nthreads; i++) {
				pthread_join(t[i], NULL);
				sum += total[i];
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			double ns = ((double) (t1.tv_sec - t0.tv_sec) * 1e9 + (double) (t1.tv_nsec - t0.tv_nsec)) * nthreads / (double) sum;
			if (ns < best[o]) {
				best[o] = ns;
			}
		}
	}
	nocopy_stats_t st;
	nocopy_stats(&st);
	printf("%u keys, %d thr, batch %d:", nkeys, nthreads, batch);
	for (int o = 0; o < nops; o++) {
		printf(" %s=%.1f", argv[7 + o], best[o]);
	}
	printf(" ns/op per thread (%llu entries, %u shards)\n", (unsigned long long) st.entries, st.shards);
	nocopy_release(&mc);
	return 0;
}
