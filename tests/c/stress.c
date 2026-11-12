/* Threaded stress test of the arena without PHP: build with tests/c/run.sh (plain -O2 and ThreadSanitizer).
 * Each thread writes, deletes and reads its own keys against a model, reads every other thread's keys,
 * and holds some values pinned across later operations: a value read must always be intact.
 * All threads also bump shared counters in place (nocopy_update) and read them lock-free: none may lose an increment.
 * Many values point at an atom (a word from a pool too large for one half, so generations rotate): it must still hold its word. */
#include "../../arena/nocopy_arena.h"
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
#define CTRS 8
static uint64_t bumps; /* successful nocopy_update increments, all threads */

/* after the key; then len bytes of a pattern of (key, ver) */
typedef struct {
	uint32_t ver, len;
	const char *atom; /* NULL, or the atom of word `word` */
	uint32_t word;
} payload;

#define WORDS 400
#define WLEN 400 /* long: a half holds ~80, so generations turn over faster than values die */
static void word(char *w, uint32_t i) /* WLEN bytes and a NUL */
{
	char n[16];
	memset(w, '.', WLEN);
	w[WLEN] = 0;
	memcpy(w, n, (size_t) snprintf(n, sizeof n, "word-%05u", i));
}

static payload *pl(nocopy_entry *e);

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) from; /* moved bytes: the atom pointer still holds */
	return nocopy_entry_flags(e) ? nocopy_atom_bits(pl(e)->atom) : 0;
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

static payload *pl(nocopy_entry *e) { return (payload *) ((char *) e + HDR + NOCOPY_ALIGN(nocopy_entry_klen(e) + 1)); }

/* Checks a found entry; returns its ver or -1 if corrupt. */
static long check(nocopy_entry *e, uint64_t h, const char *k, size_t klen)
{
	payload *p = pl(e);
	const uint8_t *d = (const uint8_t *) (p + 1);
	if (nocopy_entry_klen(e) != klen || memcmp(nocopy_key(e), k, klen)) {
		return -1;
	}
	if ((char *) (d + p->len) > (char *) e + nocopy_entry_size(e)) {
		return -1;
	}
	for (uint32_t i = 0; i < p->len; i++) {
		if (d[i] != pat(h, p->ver, i)) {
			return -1;
		}
	}
	if (nocopy_entry_flags(e) && p->atom) {
		char w[WLEN + 1];
		word(w, p->word);
		if (memcmp(p->atom, w, WLEN + 1)) {
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

/* With wi < WORDS the value references that word's atom, admitted on its second sighting (as the PHP layer does). */
static bool put(nocopy_ctx *c, const char *k, uint32_t ver, uint32_t len, time_t ttl, uint32_t wi)
{
	size_t klen = strlen(k), total = HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(sizeof(payload) + len);
	uint64_t h = hash(k, klen);
	nocopy_atom_view v;
	const char *atom = NULL;
	bool viewed = wi < WORDS && nocopy_atom_begin(c, &v);
	if (viewed) {
		char w[WLEN + 1];
		word(w, wi);
		uint64_t wh = hash(w, WLEN);
		if (!(atom = nocopy_atom_find(&v, wh, w, WLEN)) && nocopy_atom_admit(wh)) {
			atom = nocopy_atom_add(&v, wh, NULL, 0, w, WLEN); /* usable right away, as the PHP layer does */
		}
	}
	nocopy_put p;
	nocopy_put_req q = {.h = h, .key = k, .klen = (uint32_t) klen, .koff = HDR, .size = total, .ttl = ttl, .flags = 1, .atoms = atom != NULL};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q); /* flags 1: a payload with an atom field */
	bool ok = false;
	if (e) {
		payload *pp = pl(e);
		pp->ver = ver;
		pp->len = len;
		pp->atom = atom;
		pp->word = wi;
		for (uint32_t i = 0; i < len; i++) {
			((uint8_t *) (pp + 1))[i] = pat(h, ver, i);
		}
		ok = nocopy_put_commit(&p, false) == NOCOPY_PUT_OK;
	}
	if (viewed) {
		nocopy_atom_end(c);
	}
	return ok;
}

static void del(const char *k) { nocopy_delete(hash(k, strlen(k)), k, (uint32_t) strlen(k)); }

static uint64_t *ctr(nocopy_entry *e) { return (uint64_t *) pl(e); }
static int bump(nocopy_entry *e, void *arg) { (void) arg; __atomic_add_fetch(ctr(e), 1, __ATOMIC_RELAXED); return 1; }

static bool put_ctr(nocopy_ctx *c, int j)
{
	char k[8];
	snprintf(k, sizeof k, "c%d", j);
	size_t klen = strlen(k);
	nocopy_put p;
	nocopy_put_req q = {.h = hash(k, klen), .key = k, .klen = (uint32_t) klen, .koff = HDR, .size = HDR + NOCOPY_ALIGN(klen + 1) + sizeof(uint64_t)};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q);
	if (!e) {
		return false;
	}
	*ctr(e) = 0;
	return nocopy_put_commit(&p, false) == NOCOPY_PUT_OK;
}

/* Bumps or reads (lock-free) a shared counter. */
static void ctr_op(nocopy_ctx *c, unsigned *seed)
{
	char k[8];
	snprintf(k, sizeof k, "c%d", rand_r(seed) % CTRS);
	uint32_t klen = (uint32_t) strlen(k);
	uint64_t h = hash(k, klen);
	if (rand_r(seed) % 2) {
		if (nocopy_update(h, k, klen, bump, NULL) == 1) {
			__atomic_add_fetch(&bumps, 1, __ATOMIC_RELAXED);
		}
		return;
	}
	nocopy_borrow_begin(c);
	nocopy_entry *e = nocopy_find(c, h, k, klen);
	if (e && (nocopy_entry_klen(e) != klen || memcmp(nocopy_key(e), k, klen))) {
		fail("corrupt counter", k);
	} else if (e) {
		(void) __atomic_load_n(ctr(e), __ATOMIC_RELAXED);
	}
	nocopy_borrow_end(c);
}

typedef struct { nocopy_entry *e; uint64_t h; char k[32]; long ver; } held;

static void *worker(void *arg)
{
	int me = (int) (intptr_t) arg;
	unsigned seed = (unsigned) me * 7919 + 1;
	nocopy_ctx c;
	nocopy_ctx_init(&c);
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
		if (rand_r(&seed) % 8 == 0) {
			ctr_op(&c, &seed);
			continue;
		}
		int i = rand_r(&seed) % KEYS, r = rand_r(&seed) % 100;
		snprintf(k, sizeof k, "t%d_%d", me, i);
		if (r < 35) {
			uint64_t ex = nocopy_expunges();
			uint32_t len = sizes[rand_r(&seed) % (r < 2 ? 6 : 5)];
			bool ttl = rand_r(&seed) % 8 == 0;
			uint32_t wi = rand_r(&seed) % 4 ? (uint32_t) (rand_r(&seed) % WORDS) : WORDS;
			if (put(&c, k, ++ver, len, ttl ? 1 + rand_r(&seed) % 2 : 0, wi)) {
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
				uint64_t ex = nocopy_expunges();
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
			nocopy_request_end(&c);
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
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = mb << 20, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name); /* this run's only: the mapping stays */
	if (err.code != NOCOPY_ERR_NONE) {
		fprintf(stderr, "%s\n", err.msg);
		return 2;
	}
	nocopy_ctx mc;
	nocopy_ctx_init(&mc);
	if (!nocopy_claim(&mc)) {
		fprintf(stderr, "claim\n");
		return 2;
	}
	nocopy_put xp; /* an absolute expiry, kept as is */
	nocopy_entry *xe = nocopy_put_begin(&xp, &mc, &(nocopy_put_req) {.h = hash("x", 1), .key = "x", .klen = 1, .koff = HDR, .size = HDR + NOCOPY_ALIGN(2), .expires = 12345});
	assert(xe && nocopy_entry_expires(xe) == 12345 && !memcmp(nocopy_key(xe), "x", 1));
	assert(nocopy_put_commit(&xp, false) == NOCOPY_PUT_OK);
	del("x");
	for (int j = 0; j < CTRS; j++) {
		assert(put_ctr(&mc, j));
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
	uint64_t sum = 0;
	int found = 0;
	nocopy_borrow_begin(&mc);
	for (int j = 0; j < CTRS; j++) {
		char k[8];
		snprintf(k, sizeof k, "c%d", j);
		nocopy_entry *e = nocopy_find(&mc, hash(k, strlen(k)), k, (uint32_t) strlen(k));
		if (e) {
			sum += *ctr(e);
			found++;
		}
	}
	nocopy_borrow_end(&mc);
	const char *bad = ar_shards_verify();
	if (bad) {
		fprintf(stderr, "FAIL invariant: %s\n", bad);
		failures++;
	}
	if (found == CTRS && sum != bumps) { /* evicted counters lose their count: only exact when all survived */
		fprintf(stderr, "FAIL counters: %llu, expected %llu\n", (unsigned long long) sum, (unsigned long long) bumps);
		failures++;
	}
	nocopy_release(&mc);
	nocopy_stats_t z; /* every thread released: a clear gives every segment back */
	nocopy_clear();
	nocopy_stats(&z);
	if (z.entries || z.used || z.dead_segments || z.pinned_segments || z.kept_segments || z.leases || z.free_segments != z.segments) {
		fprintf(stderr, "FAIL leftovers after clear: entries %llu, used %llu, dead %u, pinned %u, kept %u, leases %llu, free %u of %u\n",
			(unsigned long long) z.entries, (unsigned long long) z.used, z.dead_segments, z.pinned_segments, z.kept_segments,
			(unsigned long long) z.leases, z.free_segments, z.segments);
		failures++;
	}
	printf("%zu MB, %d threads, %ds: %llu entries, %u shards, %llu expunges, %llu MB moved, %llu bumps (%d/%d counters kept), %llu atoms, %llu rotations, failures %llu\n",
		mb, nthreads, seconds, (unsigned long long) s.entries, s.shards, (unsigned long long) s.expunges, (unsigned long long) (s.evacuated >> 20),
		(unsigned long long) bumps, found, CTRS, (unsigned long long) s.atoms, (unsigned long long) s.atom_rotations, (unsigned long long) failures);
	return failures ? 1 : 0;
}
