/* Crash injection: a forked worker runs a mixed workload and is killed at a named step of a write (AR_HOOK, -DNOCOPY_HOOKS).
 * The survivor must find a consistent arena: every value it reads intact, values it kept pinned untouched, every structure
 * as the slot tables say, the workload still running, and all memory back after a clear. Then the steps another worker can
 * take between a reclaimer finding a slot's owner dead and acting on it. Inspects internals: layout.h.
 * Usage: crash [point [hit [mb]]]: one case, to replay a failure; crash all: every kill, without the dead-slot steps. */
#include "../../arena/layout.h"
#include "../../arena/nocopy_arena.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
#define AT (&ar_hdr->atoms)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "crash: %s, %d MB: %s (line %d)\n", where, arena_mb, #c, __LINE__); exit(1); } } while (0)

static const char *points[] = {
	"put.alloc", "put.fill", "commit.born", "commit.linked", "commit.done",
	"delete.unlinked", "delete.died", "shift.odd", "shift.moved",
	"take.claimed", "take.owned", "seg.taken", "seg.sealed", "pool.claimed", "pool.given", "give.zeroed", "reclaim.gave", "expire.unlinked",
	"merge.picked", "move.alloc", "move.copied", "move.linked", "merge.moved", "merge.evicted", "rebalance.unlocked", "rebalance.merged",
	"clear.emptied", "clear.unlocked", "lease.taken",
	"atom.bytes", "atom.used", "rotate.marked", "rotate.unused", "rotate.wiped", "rotate.state", "rotate.gen",
};
static const int hits[] = {1, 2, 3, 5, 9, 17, 40, 150, 700};

static char where[96] = "setup";
static int arena_mb;
static uint32_t nkeys, len_div; /* values up to a segment / len_div */
static int child_ops;
static time_t fake_now = 1700000000;
static time_t fake_clock(void) { return fake_now; }

/* ---- values: after the key, a payload whose bytes follow from the key and its version; some point at an atom ---- */

typedef struct {
	uint32_t ver, len;
	const char *atom; /* NULL, or the atom of word `word` */
	uint32_t word;
} payload;

#define WORDS 300
#define NOWORD UINT32_MAX
static uint32_t fresh = 1000000; /* words no value has used yet */
#define WLEN 400 /* long: a half holds few, so generations rotate */
static void word(char *w, uint32_t i)
{
	char n[16];
	memset(w, '.', WLEN);
	w[WLEN] = 0;
	memcpy(w, n, (size_t) snprintf(n, sizeof n, "word-%05u", i));
}

static payload *pl(const nocopy_entry *e) { return (payload *) ((char *) e + HDR + NOCOPY_ALIGN(nocopy_entry_klen(e) + 1)); }

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) from;
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

static bool intact(const nocopy_entry *e, const char *k)
{
	size_t klen = strlen(k);
	uint64_t h = hash(k, klen);
	const payload *p = pl(e);
	const uint8_t *d = (const uint8_t *) (p + 1);
	if (nocopy_entry_klen(e) != klen || memcmp(nocopy_key(e), k, klen) || (const char *) (d + p->len) > (const char *) e + nocopy_entry_size(e)) {
		return false;
	}
	if (nocopy_entry_flags(e) == 2) {
		return true; /* a counter */
	}
	for (uint32_t i = 0; i < p->len; i++) {
		if (d[i] != pat(h, p->ver, i)) {
			return false;
		}
	}
	if (p->atom) {
		char w[WLEN + 1];
		word(w, p->word);
		return !memcmp(p->atom, w, WLEN + 1);
	}
	return true;
}

static bool put(nocopy_ctx *c, const char *k, uint32_t ver, uint32_t len, time_t ttl, uint32_t wi)
{
	size_t klen = strlen(k), total = HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(sizeof(payload) + len);
	uint64_t h = hash(k, klen);
	nocopy_atom_view v;
	const char *atom = NULL;
	bool viewed = wi != NOWORD && nocopy_atom_begin(c, &v);
	if (viewed) {
		char w[WLEN + 1];
		word(w, wi);
		uint64_t wh = hash(w, WLEN);
		if (!(atom = nocopy_atom_find(&v, wh, w, WLEN)) && nocopy_atom_admit(wh)) {
			atom = nocopy_atom_add(&v, wh, NULL, 0, w, WLEN);
		}
	}
	nocopy_put p;
	nocopy_put_req q = {.h = h, .key = k, .klen = (uint32_t) klen, .koff = HDR, .size = total, .ttl = ttl, .flags = 1, .deps = atom ? nocopy_atom_bit(&v) : 0};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q);
	bool ok = false;
	if (e) {
		payload *pp = pl(e);
		*pp = (payload) {.ver = ver, .len = len, .atom = atom, .word = wi};
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

static int bump(nocopy_entry *e, void *arg)
{
	(void) arg;
	__atomic_add_fetch(&pl(e)->ver, 1, __ATOMIC_RELAXED);
	return 1;
}

static void count(nocopy_ctx *c, const char *k)
{
	size_t klen = strlen(k);
	uint64_t h = hash(k, klen);
	if (nocopy_update(h, k, (uint32_t) klen, bump, NULL) != NOCOPY_GONE) {
		return;
	}
	nocopy_put p;
	nocopy_put_req q = {.h = h, .key = k, .klen = (uint32_t) klen, .koff = HDR, .size = HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(sizeof(payload)), .flags = 2};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q);
	if (e) {
		*pl(e) = (payload) {0};
		nocopy_put_commit(&p, true);
	}
}

/* ---- the workload ---- */

static uint32_t rng;
static uint32_t rnd(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}

/* false: a value read was corrupt. Keys lean on one shard, then on another: a shard over its share, one that must take it back. */
static bool workload(nocopy_ctx *c, uint32_t seed, int ops)
{
	rng = seed | 1;
	uint32_t hot = rnd() & ar_shard_mask;
	char k[24];
	for (int n = 0; n < ops; n++) {
		if (n == ops / 2) {
			hot = (hot + 1) & ar_shard_mask;
		}
		uint32_t r = rnd() % 100, ki = rnd() % nkeys;
		snprintf(k, sizeof k, "k%u", ki);
		for (int t = 0; t < 6 && rnd() % 10 < 9 && ar_shard_idx(hash(k, strlen(k))) != hot; t++) {
			snprintf(k, sizeof k, "k%u", ki = rnd() % nkeys);
		}
		if (r < 45) {
			put(c, k, rnd(), 100 + rnd() % (uint32_t) (ar_seg_size / len_div), rnd() % 5 < 4 ? 0 : 2 + rnd() % 30, rnd() % 10 < 3 ? rnd() % WORDS : NOWORD);
		} else if (r < 49) {
			snprintf(k, sizeof k, "b%u", rnd() % 8); /* a run of segments */
			put(c, k, rnd(), (uint32_t) (ar_seg_size / 2 + rnd() % (2 * ar_seg_size)), rnd() % 2 ? 0 : 5, NOWORD);
		} else if (r < 62) {
			nocopy_delete(hash(k, strlen(k)), k, (uint32_t) strlen(k));
		} else if (r < 80) {
			nocopy_borrow_begin(c);
			nocopy_entry *e = nocopy_find(c, hash(k, strlen(k)), k, (uint32_t) strlen(k));
			bool ok = !e || intact(e, k);
			nocopy_borrow_end(c);
			if (!ok) {
				fprintf(stderr, "crash: corrupt value under %s\n", k);
				return false;
			}
		} else if (r < 84) {
			snprintf(k, sizeof k, "c%u", rnd() % 4);
			count(c, k);
		} else if (r < 88) {
			fake_now += 1 + rnd() % 3; /* TTLs pass, once-a-second work comes due */
		} else if (r < 90) {
			uint64_t h = hash(k, strlen(k));
			if (nocopy_lease_take(h, nocopy_ctx_slot(c), fake_now, 30) == NOCOPY_LEASE_TAKEN && rnd() % 4) {
				nocopy_lease_drop(h, nocopy_ctx_slot(c));
			}
		} else if (r < 91) {
			if (rnd() % 8 == 0) {
				nocopy_clear();
			}
		} else if (r < 92) {
			if (rnd() % 4 == 0) { /* nothing references the other half, this one fills: a rotation goes through */
				nocopy_clear();
				for (uint32_t i = 0; i < AT->half_bytes / WLEN + 8; i++) {
					put(c, "r", rnd(), 64, 0, fresh);
					put(c, "r", rnd(), 64, 0, fresh++);
				}
			}
		} else {
			uint32_t wi = rnd() % WORDS; /* twice in a row admits its word: halves fill and rotate */
			put(c, k, rnd(), 64, 0, wi);
			put(c, k, rnd(), 64, 0, wi);
		}
	}
	return true;
}

/* ---- what must hold with no write in progress ---- */


static void structure(void)
{
	for (uint32_t si = 0; si <= ar_shard_mask; si++) {
		CHECK(!ar_shards[si].lock.holder);
	}
	const char *bad = ar_shards_verify();
	if (bad) {
		fprintf(stderr, "crash: %s\n", bad);
	}
	CHECK(!bad);
	/* atoms: the current generation's table reaches whole records only */
	uint32_t g = AT->gen;
	const ar_atom_half *hf = &AT->half[g & 1];
	CHECK(hf->state == g && hf->used <= AT->half_bytes && !AT->lock.holder);
	for (uint32_t i = 0; i <= ar_atom_mask; i++) {
		ar_slot sl = ar_atom_tab[(size_t) (g & 1) * (ar_atom_mask + 1) + i];
		if (sl) {
			uint32_t at = (uint32_t) sl << 3;
			const uint32_t *r = (const uint32_t *) (ar_atoms + (size_t) (g & 1) * ar_atom_bytes + at);
			CHECK(at >= 8 && at < hf->used && at + 8 + r[1] + r[0] + 1 <= hf->used);
		}
	}
}

static void read_all(nocopy_ctx *c)
{
	char k[24];
	for (uint32_t i = 0; i < nkeys + 12; i++) {
		if (i < nkeys) {
			snprintf(k, sizeof k, "k%u", i);
		} else {
			snprintf(k, sizeof k, "%c%u", i < nkeys + 8 ? 'b' : 'c', (i - nkeys) % 8);
		}
		nocopy_borrow_begin(c);
		nocopy_entry *e = nocopy_find(c, hash(k, strlen(k)), k, (uint32_t) strlen(k));
		CHECK(!e || intact(e, k));
		nocopy_borrow_end(c);
	}
}

/* Every lock a dead worker may hold is taken once (stolen, its shard repaired); once-a-second liveness checks come due. */
static void touch_all(nocopy_ctx *c)
{
	char k[24];
	uint32_t seen = 0;
	fake_now += 2;
	for (uint32_t i = 0; seen != (1u << (ar_shard_mask + 1)) - 1; i++) {
		snprintf(k, sizeof k, "probe%u", i);
		uint64_t h = hash(k, strlen(k));
		uint32_t s = ar_shard_idx(h); /* its lease table too */
		if (seen & 1u << s) {
			continue;
		}
		seen |= 1u << s;
		put(c, k, 1, 50, 0, NOWORD);
		nocopy_delete(h, k, (uint32_t) strlen(k));
		if (nocopy_lease_take(h, nocopy_ctx_slot(c), fake_now, 30) == NOCOPY_LEASE_TAKEN) {
			nocopy_lease_drop(h, nocopy_ctx_slot(c));
		}
	}
	for (uint32_t i = 0; i < 4; i++) { /* the atom lock: a trylock that looks at a dead holder once a second */
		fake_now += 2;
		snprintf(k, sizeof k, "probe%u", i);
		put(c, k, 1, 50, 0, fresh);
		put(c, k, 1, 50, 0, fresh++);
		nocopy_delete(hash(k, strlen(k)), k, (uint32_t) strlen(k));
	}
}

/* ---- one case ---- */

static const char *die_at;
static int die_in;
static void die_hook(const char *point)
{
	if (!strcmp(point, die_at) && --die_in == 0) {
		kill(getpid(), SIGKILL);
	}
}

#define KEPT 6
static bool run_case(nocopy_ctx *c, const char *point, int hit, uint32_t seed)
{
	snprintf(where, sizeof where, "%s#%d seed %u", point, hit, seed);
	/* values the survivor holds zero-copy across the other worker's death */
	const nocopy_entry *kept[KEPT];
	char kk[KEPT][24];
	int nkept = 0;
	CHECK(workload(c, seed ^ 0x5bd1e995, 1500));
	nocopy_borrow_begin(c);
	for (uint32_t i = 0; i < nkeys && nkept < KEPT; i += nkeys / 40 + 1) {
		snprintf(kk[nkept], sizeof kk[0], "k%u", i);
		nocopy_entry *e = nocopy_find(c, hash(kk[nkept], strlen(kk[nkept])), kk[nkept], (uint32_t) strlen(kk[nkept]));
		if (e) {
			nocopy_keep(c, e);
			kept[nkept++] = e;
		}
	}
	nocopy_borrow_end(c);

	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		nocopy_after_fork(c);
		fresh += 1u << 24; /* its own words: the survivor's must be new to the arena */
		die_at = point;
		die_in = hit;
		ar_hook = die_hook;
		_exit(workload(c, seed, child_ops) ? 0 : 2);
	}
	int st;
	CHECK(waitpid(pid, &st, 0) == pid);
	bool died = WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
	CHECK(died || (WIFEXITED(st) && WEXITSTATUS(st) == 0)); /* 2: the worker itself read a corrupt value */

	alarm(60); /* a lock nobody can take, a probe that never ends */
	for (int i = 0; i < nkept; i++) {
		CHECK(intact(kept[i], kk[i]));
	}
	touch_all(c);
	for (int i = 0; i < nkept; i++) {
		CHECK(intact(kept[i], kk[i]));
	}
	read_all(c);
	structure();
	CHECK(workload(c, seed ^ 0x9e3779b9, 3000));
	for (int i = 0; i < nkept; i++) {
		CHECK(intact(kept[i], kk[i]));
	}
	read_all(c);
	structure();
	/* nothing is lost for good: with no reader left, a clear gets every segment back and the atoms can turn over */
	nocopy_request_end(c);
	fake_now += 2;
	nocopy_clear();
	fake_now += 2;
	nocopy_clear();
	structure();
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		CHECK(!ar_seg_owner[s] && ar_segs[s].state == SEG_FREE && !ar_segs[s].pending && !ar_segmeta_at[s].deps);
	}
	uint64_t rot = AT->rotations;
	char k[24];
	for (uint32_t i = 0; i < 4 * (AT->half_bytes / WLEN) && AT->rotations < rot + 2; i++) {
		fake_now += 2;
		snprintf(k, sizeof k, "k%u", i % 7);
		put(c, k, 1, 64, 0, fresh);
		put(c, k, 1, 64, 0, fresh++);
		nocopy_delete(hash(k, strlen(k)), k, (uint32_t) strlen(k));
		if (i % 16 == 15) {
			nocopy_clear();
		}
	}
	CHECK(AT->rotations >= rot + 2); /* both halves were wiped: no reference to either is stuck */
	nocopy_clear();
	alarm(0);
	return died;
}

static void open_arena(int mb, uint32_t keys, uint32_t div, int ops, nocopy_ctx *c)
{
	len_div = div;
	child_ops = ops;
	char base[40];
	nocopy_named nm;
	if (ar_base) {
		nocopy_arena_close();
	}
	arena_mb = mb;
	nkeys = keys;
	snprintf(base, sizeof base, "/nocopy-crash-%d-%d", (int) getpid(), mb);
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = (size_t) mb << 20, .hint = keys, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name);
	CHECK(err.code == NOCOPY_ERR_NONE);
	nocopy_ctx_init(c);
	CHECK(nocopy_claim(c));
}

/* ---- a slot whose owner died, claimed by a new worker while another acts on the death ---- */

static uint64_t dead_owner(void) /* the id of a process that has exited and been reaped: alive until then */
{
	int up[2], down[2];
	assert(pipe(up) == 0 && pipe(down) == 0);
	pid_t p = fork();
	if (!p) {
		char c;
		close(down[1]);
		ar_proc_attach(ar_region_reopen());
		if (write(up[1], &ar_self, sizeof ar_self) == sizeof ar_self) {
			(void) !read(down[0], &c, 1);
		}
		_exit(0);
	}
	uint64_t id = 0;
	assert(read(up[0], &id, sizeof id) == sizeof id && id && id != ar_self && !ar_owner_dead(id));
	close(down[1]);
	waitpid(p, NULL, 0);
	assert(ar_owner_dead(id));
	close(up[0]);
	close(up[1]);
	close(down[0]);
	return id;
}

static nocopy_ctx newcomer;
static const char *step_at;
static uint32_t newcomer_seg;
static bool stepped;

/* The newcomer's steps, run where the other worker has just found the slot's owner dead. */
static void claim_hook(const char *point)
{
	if (stepped || strcmp(point, step_at)) {
		return;
	}
	stepped = true;
	nocopy_ctx_init(&newcomer);
	CHECK(nocopy_claim(&newcomer));
	if (!strcmp(point, "pins.dead")) {
		ar_pin(&newcomer.pins, ar_seg_start(newcomer_seg)); /* a read: its segment published as pinned */
	} else {
		nocopy_atom_view v;
		CHECK(nocopy_atom_begin(&newcomer, &v)); /* a write: its generation announced */
	}
}

static void full_house(void) /* no free slot: a newcomer takes a dead owner's */
{
	for (uint32_t i = 0; i < NOCOPY_MAX_WORKERS; i++) {
		if (!ar_worker_at(i)->owner) {
			ar_worker_at(i)->owner = ar_self;
		}
	}
	__atomic_store_n(&ar_proc->nworkers, NOCOPY_MAX_WORKERS, __ATOMIC_RELEASE);
}

static void dead_slot_races(void)
{
	nocopy_ctx c, d;
	char k[24];
	snprintf(where, sizeof where, "dead slot: pins");
	open_arena(4, 400, 10, 0, &c);
	nocopy_set_clock(fake_clock);
	nocopy_ctx_init(&d);
	CHECK(nocopy_claim(&d));
	for (uint32_t i = 0; i < 300; i++) {
		snprintf(k, sizeof k, "k%u", i);
		CHECK(put(&c, k, 1, 2000, 0, NOWORD));
	}
	nocopy_borrow_begin(&d);
	nocopy_entry *e = nocopy_find(&d, hash("k1", 2), "k1", 2);
	CHECK(e);
	nocopy_borrow_end(&d);
	uint32_t slot = (uint32_t) nocopy_ctx_slot(&d);
	newcomer_seg = (ar_seg_of(e) + 5) % ar_nsegs;
	ar_worker_at(slot)->owner = dead_owner(); /* d dies with its pin */
	full_house();
	step_at = "pins.dead";
	stepped = false;
	ar_hook = claim_hook;
	fake_now += 2;
	nocopy_clear(); /* its reclaim meets the dead worker's pin on a dead segment */
	ar_hook = NULL;
	CHECK(stepped && (uint32_t) nocopy_ctx_slot(&newcomer) == slot);
	CHECK(ar_pins_of((int) slot)[newcomer_seg >> 6] & ar_segbit(newcomer_seg)); /* the newcomer's pin is still published */

	snprintf(where, sizeof where, "dead slot: atom view");
	open_arena(4, 400, 10, 0, &c);
	nocopy_ctx_init(&d);
	CHECK(nocopy_claim(&d));
	nocopy_atom_view v;
	CHECK(nocopy_atom_begin(&d, &v));
	slot = (uint32_t) nocopy_ctx_slot(&d);
	ar_worker_at(slot)->owner = dead_owner(); /* d dies in a write, its generation announced */
	full_house();
	step_at = "view.dead";
	stepped = false;
	ar_hook = claim_hook;
	uint32_t g0 = AT->gen;
	for (uint32_t i = 0; i < 4000 && !stepped; i++) { /* two rotations on: the second asks who holds d's generation */
		fake_now += 2;
		snprintf(k, sizeof k, "w%u", i);
		nocopy_atom_view av;
		CHECK(nocopy_atom_begin(&c, &av));
		char w[WLEN + 1];
		word(w, 100000 + i);
		nocopy_atom_add(&av, hash(w, WLEN), NULL, 0, w, WLEN);
		nocopy_atom_end(&c);
	}
	ar_hook = NULL;
	CHECK(stepped && AT->gen > g0 && (uint32_t) nocopy_ctx_slot(&newcomer) == slot);
	CHECK(ar_worker_at(slot)->agen == newcomer.atom.agen && newcomer.atom.agen); /* the newcomer's view is still announced */
	nocopy_arena_close();
}

int main(int argc, char **argv)
{
	nocopy_ctx c;
	int cases = 0, died = 0;
	if (argc > 1 && !strcmp(argv[1], "all")) { /* crash all: the kills without the dead-slot steps */
		argc = 1;
	} else if (argc == 1 || !strcmp(argv[1], "slots")) {
		dead_slot_races();
		if (argc > 1) {
			printf("crash: dead slots ok\n");
			return 0;
		}
	}
	static const struct { int mb; uint32_t keys, div; int ops; } arenas[] = {{4, 3000, 10, 6000}, {16, 9000, 3, 10000}}; /* one shard; two, which balance */
	bool reached[sizeof points / sizeof *points] = {false};
	for (size_t a = 0; a < sizeof arenas / sizeof *arenas; a++) {
		if (argc > 3 && atoi(argv[3]) != arenas[a].mb) {
			continue;
		}
		open_arena(arenas[a].mb, arenas[a].keys, arenas[a].div, arenas[a].ops, &c);
		nocopy_set_clock(fake_clock);
		for (size_t p = 0; p < sizeof points / sizeof *points; p++) {
			int at = 0;
			for (size_t h = 0; h < sizeof hits / sizeof *hits; h++) {
				if ((argc > 1 && strcmp(argv[1], points[p])) || (argc > 2 && atoi(argv[2]) != hits[h])) {
					continue;
				}
				cases++;
				at += run_case(&c, points[p], hits[h], (uint32_t) (p * 131 + h * 7 + a + 1));
			}
			died += at;
			reached[p] |= at > 0;
			if (getenv("CRASH_VERBOSE")) {
				printf("%d MB %s: %d\n", arenas[a].mb, points[p], at);
			}
		}
	}
	for (size_t p = 0; argc == 1 && p < sizeof points / sizeof *points; p++) {
		if (!reached[p]) {
			printf("crash: no worker was killed at %s\n", points[p]); /* ponytail: the workload does not get there with these seeds */
		}
	}
	printf("crash: ok (%d cases, the worker died in %d)\n", cases, died);
	return 0;
}
