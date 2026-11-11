/* Interleaving explorer: 2-3 threads run a scenario one at a time, switching only at the arena's named steps (AR_HOOK,
 * -DNOCOPY_HOOKS), and every order of those steps with at most `bound` preemptions is run, each on a fresh arena.
 * A reader must never see a freed or reused block, never miss a key that is there, never see a version go back.
 * Sequentially consistent orders only: a missing fence is ThreadSanitizer's (stress.c), not this test's.
 * Usage: sched [scenario [path]]: one scenario; with a path (as a failure prints it) that one order, traced. */
#include "../../arena/layout.h"
#include "../../arena/nocopy_arena.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
#define MAXT 3
#define MAXD 4096      /* decisions in one run */
#define MAXSTEPS 20000 /* steps in one run: more is a livelock */

/* ---- the scheduler ---- */

typedef struct {
	const char *name;
	int mb, bound, nthreads;
	void (*setup)(nocopy_ctx *c);
	void (*body[MAXT])(nocopy_ctx *c);
	void (*check)(nocopy_ctx *c);
	const char *must[6]; /* steps some order has to reach, or the scenario tests nothing */
} scenario;

static const scenario *S;
static struct {
	pthread_t th;
	nocopy_ctx c;
	bool done;
	const ar_lock *lock;   /* waits for it to be free */
	const ar_probe *probe; /* waits for the shift it met to end */
} T[MAXT];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int cur; /* whose turn: a thread, -1 main */
static __thread int me = -1;

static uint8_t path[MAXD], nalt[MAXD];
static int plen, depth, preempts, nsteps, bound;
static bool verbose;
static struct { int8_t t; const char *at; } trace[MAXSTEPS];
static struct { const char *at; uint64_t n; } seen[96];
static time_t fake_now = 1700000000;
static time_t fake_clock(void) { return fake_now; }

static void fail(const char *what)
{
	fprintf(stderr, "sched: %s: %s\nreplay: sched %s ", S->name, what, S->name);
	for (int i = 0; i < depth; i++) {
		fprintf(stderr, "%s%d", i ? "," : "", path[i]);
	}
	fprintf(stderr, "\n");
	for (int i = nsteps > 60 ? nsteps - 60 : 0; i < nsteps; i++) {
		fprintf(stderr, "  T%d %s\n", trace[i].t, trace[i].at);
	}
	exit(1);
}
#define CHECK(c) do { if (!(c)) { fail(#c); } } while (0)

static void count(const char *at)
{
	for (size_t i = 0; i < sizeof seen / sizeof *seen; i++) {
		if (!seen[i].at || !strcmp(seen[i].at, at)) {
			seen[i].at = at;
			seen[i].n++;
			return;
		}
	}
	fail("seen[] full");
}

/* Waiting for something only another thread can do: not runnable, or every spin of the wait would be an order of its own. */
static bool blocked(int t)
{
	const ar_probe *p = T[t].probe;
	return (T[t].lock && __atomic_load_n(&T[t].lock->holder, __ATOMIC_RELAXED))
		|| (p && (p->seq & 1) && __atomic_load_n(p->mseq, __ATOMIC_RELAXED) == p->seq);
}

static bool runnable(int t) { return !T[t].done && !blocked(t); }

/* Who runs next: `from` (main: -1) goes on, or another thread does. Leaving a thread that could go on is a preemption. */
static int choose(int from)
{
	int opt[MAXT], n = 0;
	bool stay = from >= 0 && runnable(from);
	if (stay) {
		opt[n++] = from;
	}
	if (!stay || preempts < bound) {
		for (int t = 0; t < S->nthreads; t++) {
			if (t != from && runnable(t)) {
				opt[n++] = t;
			}
		}
	}
	if (!n) {
		for (int t = 0; t < S->nthreads; t++) {
			if (!T[t].done) {
				fail("deadlock");
			}
		}
		return -1; /* all done: main */
	}
	int pick = 0;
	if (n > 1) {
		if (depth == MAXD) {
			fail("too many decisions");
		}
		pick = depth < plen ? path[depth] : 0;
		if (pick >= n) {
			fail("a replay took another course: the scenario is not deterministic, or the bound differs");
		}
		path[depth] = (uint8_t) pick;
		nalt[depth++] = (uint8_t) n;
	}
	preempts += stay && opt[pick] != from;
	return opt[pick];
}

static void turn(int to)
{
	pthread_mutex_lock(&mu);
	cur = to;
	pthread_cond_broadcast(&cv);
	while (me < 0 ? cur != -1 : !T[me].done && cur != me) {
		pthread_cond_wait(&cv, &mu);
	}
	pthread_mutex_unlock(&mu);
}

/* A step of a scheduled thread: the hook of every AR_HOOK, and of a scenario's own steps. */
static void step(const char *at)
{
	if (me < 0) {
		return; /* setup and checks run alone */
	}
	if (nsteps == MAXSTEPS) {
		fail("livelock");
	}
	trace[nsteps].t = (int8_t) me;
	trace[nsteps++].at = at;
	count(at);
	if (verbose) {
		printf("  T%d %s\n", me, at);
	}
	if (!strcmp(at, "lock.wait")) {
		T[me].lock = ar_hook_on;
	} else if (!strcmp(at, "probe.retry")) {
		T[me].probe = ar_hook_on;
	}
	int to = choose(me);
	if (to != me) {
		turn(to);
	}
	T[me].lock = NULL;
	T[me].probe = NULL;
}

static void *thread(void *arg)
{
	me = (int) (intptr_t) arg;
	pthread_mutex_lock(&mu);
	while (cur != me) {
		pthread_cond_wait(&cv, &mu);
	}
	pthread_mutex_unlock(&mu);
	S->body[me](&T[me].c);
	T[me].done = true;
	turn(choose(me));
	return NULL;
}

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from) { (void) e; (void) from; return 0; }

/* One order: a fresh arena, so a replayed prefix meets the same state. */
static void run(void)
{
	char base[40];
	nocopy_named nm;
	nocopy_ctx c;
	snprintf(base, sizeof base, "/nocopy-sched-%d", (int) getpid());
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = (size_t) S->mb << 20, .hint = 1000, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name);
	if (err.code != NOCOPY_ERR_NONE) {
		fprintf(stderr, "sched: no arena: %s\n", err.msg);
		exit(1);
	}
	nocopy_set_clock(fake_clock);
	depth = preempts = nsteps = 0;
	nocopy_ctx_init(&c);
	CHECK(nocopy_claim(&c));
	c.rng = 1;
	for (int t = 0; t < S->nthreads; t++) {
		nocopy_ctx_init(&T[t].c);
		CHECK(nocopy_claim(&T[t].c));
		T[t].c.rng = (uint32_t) t * 2 + 3; /* claim seeds it from the owner id */
		T[t].done = false;
		T[t].lock = NULL;
		T[t].probe = NULL;
	}
	S->setup(&c);
	cur = -2;
	for (int t = 0; t < S->nthreads; t++) {
		pthread_create(&T[t].th, NULL, thread, (void *) (intptr_t) t);
	}
	turn(choose(-1));
	for (int t = 0; t < S->nthreads; t++) {
		pthread_join(T[t].th, NULL);
	}
	if (depth < plen) {
		fail("a replay took another course: it made fewer decisions");
	}
	S->check(&c);
	nocopy_arena_close();
}

/* Depth first over the decisions: replay a prefix, take the first option after it, then advance the deepest decision that has one left. */
static uint64_t explore(void)
{
	uint64_t runs = 0;
	plen = 0;
	for (;;) {
		run();
		runs++;
		int d = depth;
		while (d && path[d - 1] + 1 >= nalt[d - 1]) {
			d--;
		}
		if (!d) {
			return runs;
		}
		path[d - 1]++;
		plen = d;
	}
}

/* ---- values: a key is its hash in hex; the payload's bytes follow from the hash and a version ---- */

typedef struct {
	uint32_t ver, len;
} payload;

static int keyof(char *k, uint64_t h) { return snprintf(k, 20, "%llx", (unsigned long long) h); }
static payload *pl(const nocopy_entry *e) { return (payload *) ((char *) e + HDR + NOCOPY_ALIGN(nocopy_entry_klen(e) + 1)); }
static uint8_t pat(uint64_t h, uint32_t ver, uint32_t i) { return (uint8_t) (h ^ ver * 31 ^ i * 7); }

static bool put(nocopy_ctx *c, uint64_t h, uint32_t ver, uint32_t len)
{
	char k[20];
	uint32_t klen = (uint32_t) keyof(k, h);
	nocopy_put p;
	nocopy_put_req q = {.h = h, .key = k, .klen = klen, .koff = HDR, .size = HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(sizeof(payload) + len)};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q);
	if (!e) {
		return false;
	}
	*pl(e) = (payload) {ver, len};
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *) (pl(e) + 1))[i] = pat(h, ver, i);
	}
	return nocopy_put_commit(&p, false) == NOCOPY_PUT_OK;
}

static void del(uint64_t h)
{
	char k[20];
	nocopy_delete(h, k, (uint32_t) keyof(k, h));
}

#define MISS (-2)
#define CORRUPT (-1)

/* A found entry's version, or CORRUPT: freed blocks are poisoned (-DNOCOPY_POISON), reused ones hold another key or version. */
static long ver_of(const nocopy_entry *e, uint64_t h)
{
	char k[20];
	uint32_t klen = (uint32_t) keyof(k, h);
	const payload *p = pl(e);
	const uint8_t *d = (const uint8_t *) (p + 1);
	if (nocopy_entry_klen(e) != klen || memcmp(nocopy_key(e), k, klen) || (const char *) (d + p->len) > (const char *) e + nocopy_entry_size(e)) {
		return CORRUPT;
	}
	for (uint32_t i = 0; i < p->len; i++) {
		if (d[i] != pat(h, p->ver, i)) {
			return CORRUPT;
		}
	}
	return p->ver;
}

static nocopy_entry *find(nocopy_ctx *c, uint64_t h)
{
	char k[20];
	return nocopy_find(c, h, k, (uint32_t) keyof(k, h));
}

static long get(nocopy_ctx *c, uint64_t h)
{
	nocopy_borrow_begin(c);
	nocopy_entry *e = find(c, h);
	long v = e ? ver_of(e, h) : MISS;
	nocopy_borrow_end(c);
	return v;
}

/* Finds h, holds the pointer over two steps of its own and expects the block untouched meanwhile; the version, or MISS. */
static long hold(nocopy_ctx *c, uint64_t h)
{
	nocopy_borrow_begin(c);
	nocopy_entry *e = find(c, h);
	long v = e ? ver_of(e, h) : MISS;
	CHECK(v != CORRUPT);
	for (int i = 0; e && i < 2; i++) {
		step("user.hold");
		CHECK(ver_of(e, h) == v);
	}
	nocopy_borrow_end(c);
	return v;
}

/* The next hash after `after` in like's shard and with its home slot: their slots form one probe run. */
static uint64_t same_home(uint64_t after, uint64_t like)
{
	uint32_t sh = ar_shard_idx(like), mask = ar_ix[sh].mask;
	for (uint64_t h = after + 2;; h += 2) {
		if (ar_shard_idx(h) == sh && ((ar_slot_tag(h) ^ ar_slot_tag(like)) & mask) == 0) {
			return h;
		}
	}
}

static void counted(const nocopy_entry *e, void *arg) { (void) e; ++*(uint32_t *) arg; }
static uint32_t entries(nocopy_ctx *c)
{
	uint32_t n = 0;
	nocopy_borrow_begin(c);
	nocopy_each(c, counted, &n);
	nocopy_borrow_end(c);
	return n;
}

static uint64_t A, B, C;
static void run_keys(nocopy_ctx *c)
{
	A = 0x1001;
	B = same_home(A, A);
	C = same_home(B, A);
	CHECK(put(c, A, 1, 16) && put(c, B, 1, 16) && put(c, C, 1, 16));
}

/* ---- shift: a delete moves the keys behind it back while a reader probes the run ---- */

static void shift_writer(nocopy_ctx *c)
{
	del(A);
	CHECK(put(c, A, 2, 16)); /* back in, at the run's end */
}
static void shift_reader(nocopy_ctx *c)
{
	CHECK(get(c, C) == 1);
	CHECK(get(c, B) == 1);
	long a = get(c, A);
	CHECK(a == 1 || a == 2 || a == MISS);
}
static void shift_check(nocopy_ctx *c)
{
	CHECK(get(c, A) == 2 && get(c, B) == 1 && get(c, C) == 1 && entries(c) == 3);
}

/* ---- reuse: a reader holds a value while writers replace it twice; its segment dies and is given back ---- */

static uint32_t big; /* over half a segment: a run of its own, filled outside the lock */
static void reuse_setup(nocopy_ctx *c)
{
	A = 0x1001;
	big = (uint32_t) (ar_seg_size * 3 / 4);
	CHECK(put(c, A, 1, big));
}
static void reuse_writer(nocopy_ctx *c) { CHECK(put(c, A, 2, big) && put(c, A, 3, big)); }
static void reuse_reader(nocopy_ctx *c)
{
	long v = hold(c, A), w = hold(c, A);
	CHECK(v >= 1 && w >= v && w <= 3);
}
static void reuse_check(nocopy_ctx *c) { CHECK(get(c, A) == 3 && entries(c) == 1); }

/* ---- writers: two writers of one key (one filling outside the lock), a reader between them ---- */

static void writers_a(nocopy_ctx *c) { CHECK(put(c, A, 10, 16)); }
static void writers_b(nocopy_ctx *c) { CHECK(put(c, A, 20, NOCOPY_INLINE_PUT + 64)); }
static void writers_reader(nocopy_ctx *c)
{
	long v = hold(c, A), w = hold(c, A);
	CHECK(v == 1 || v == 10 || v == 20);
	CHECK(w == 10 || w == 20 || (w == 1 && v == 1));
	CHECK(get(c, B) == 1 && get(c, C) == 1);
}
static void writers_check(nocopy_ctx *c)
{
	long a = get(c, A);
	CHECK((a == 10 || a == 20) && get(c, B) == 1 && get(c, C) == 1 && entries(c) == 3);
}

/* ---- churn: deletes and a put in one probe run, a reader of the key that stays ---- */

static void churn_del(nocopy_ctx *c) { (void) c; del(A); del(B); }
static void churn_put(nocopy_ctx *c) { CHECK(put(c, A, 2, 16)); }
static void churn_reader(nocopy_ctx *c)
{
	CHECK(hold(c, C) == 1);
	long a = get(c, A);
	CHECK(a == 1 || a == 2 || a == MISS);
	CHECK(get(c, C) == 1);
}
static void churn_check(nocopy_ctx *c)
{
	long a = get(c, A);
	CHECK((a == 2 || a == MISS) && get(c, B) == MISS && get(c, C) == 1 && entries(c) == (a == 2 ? 2u : 1u));
}

/* ---- merge: a full arena with two sparse segments; the writer's next segment comes from compacting them while a reader holds a key in one ---- */

static uint32_t part; /* four to a segment */
static bool alone(nocopy_ctx *c, uint64_t h)
{
	nocopy_borrow_begin(c);
	nocopy_entry *e = find(c, h);
	bool r = e && ar_segs[ar_seg_of(e)].live == nocopy_entry_size(e);
	nocopy_borrow_end(c);
	return r;
}
static void merge_setup(nocopy_ctx *c)
{
	part = (uint32_t) (ar_seg_size / 5);
	uint64_t h = 0x2001, x = nocopy_expunges();
	uint32_t period = 0; /* puts from one evicting merge to the next */
	for (int round = 0; round < 2; round++, x = nocopy_expunges()) {
		for (period = 0; nocopy_expunges() == x; period++, h += 2) {
			CHECK(h < 0x200000);
			CHECK(put(c, h, 1, part));
		}
	}
	for (uint32_t i = 0; i + 3 < period; i++, h += 2) { /* the next merge is due within the writer's puts */
		CHECK(put(c, h, 1, part));
	}
	A = h - 2 * 20; /* each alone in its sealed segment once its neighbours are gone */
	B = h - 2 * 16;
	for (uint64_t g = h - 2 * 23; g <= h - 2 * 13; g += 2) {
		if (g != A && g != B) {
			del(g);
		}
	}
	CHECK(nocopy_expunges() == x && get(c, A) == 1 && get(c, B) == 1);
	CHECK(alone(c, A) && alone(c, B));
}
static void merge_writer(nocopy_ctx *c)
{
	for (uint64_t h = 0x900001; h < 0x900001 + 2 * 5; h += 2) {
		CHECK(put(c, h, 1, part));
	}
}
static void merge_reader(nocopy_ctx *c)
{
	CHECK(hold(c, A) == 1); /* compaction loses nothing */
	CHECK(hold(c, B) == 1);
}
static void merge_check(nocopy_ctx *c) { CHECK(get(c, A) == 1 && get(c, B) == 1); }

static const scenario scenarios[] = {
	{"shift", 4, 3, 2, run_keys, {shift_writer, shift_reader}, shift_check, {"shift.moved", "probe.retry", "pin.set"}},
	{"reuse", 4, 3, 2, reuse_setup, {reuse_writer, reuse_reader}, reuse_check, {"reclaim.gave", "reclaim.pinned", "pin.set", "put.wip"}},
	{"writers", 4, 2, 3, run_keys, {writers_a, writers_b, writers_reader}, writers_check, {"lock.wait", "commit.relocked", "pin.set"}},
	{"churn", 4, 2, 3, run_keys, {churn_del, churn_put, churn_reader}, churn_check, {"shift.moved", "lock.wait", "probe.retry"}},
	{"merge", 4, 2, 2, merge_setup, {merge_writer, merge_reader}, merge_check, {"merge.picked", "move.linked", "reclaim.gave", "pin.set"}},
};

int main(int argc, char **argv)
{
	ar_hook = step;
	for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; i++) {
		S = &scenarios[i];
		if (argc > 1 && strcmp(argv[1], S->name)) {
			continue;
		}
		bound = getenv("SCHED_BOUND") ? atoi(getenv("SCHED_BOUND")) : S->bound; /* deeper by hand; a path replays under the bound it was found with */
		if (argc > 2) { /* one order, traced */
			for (char *p = strtok(argv[2], ","); p && plen < MAXD; p = strtok(NULL, ",")) {
				path[plen++] = (uint8_t) atoi(p);
			}
			verbose = true;
			run();
			printf("sched: %s: that order is fine\n", S->name);
			return 0;
		}
		memset(seen, 0, sizeof seen);
		uint64_t runs = explore();
		for (size_t m = 0; m < sizeof S->must / sizeof *S->must && S->must[m]; m++) {
			uint64_t n = 0;
			for (size_t j = 0; j < sizeof seen / sizeof *seen && seen[j].at; j++) {
				n += strcmp(seen[j].at, S->must[m]) ? 0 : seen[j].n;
			}
			if (!n) {
				fprintf(stderr, "sched: %s: no order reached %s\n", S->name, S->must[m]);
				return 1;
			}
		}
		printf("sched: %s ok (%llu orders, at most %d preemptions)\n", S->name, (unsigned long long) runs, bound);
	}
	return 0;
}
