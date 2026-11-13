/* Shard repair: a lock taken from a dead holder rebuilds the shard. Builds the half-done states a kill can leave
 * (a delete's shift stopped midway, an unlinked entry still counted live, an abandoned allocation, a dead writer's
 * unfinished big entry, a segment claimed and never set up) and checks the shard afterwards. Inspects internals: layout.h. */
#define _GNU_SOURCE /* F_OFD_SETLK */
#include "../../arena/layout.h"
#include "../../arena/nocopy_arena.h"
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
#define N 400

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) e, (void) from;
	return 0;
}

static uint64_t hash(const char *k)
{
	uint64_t h = 5381;
	while (*k) {
		h = h * 33 + (unsigned char) *k++;
	}
	return h | 1;
}

/* Allocated with its key set; the caller commits or leaves it. Small blocks return with the shard locked. */
static nocopy_entry *begin(nocopy_put *p, nocopy_ctx *c, const char *k, size_t len)
{
	size_t klen = strlen(k);
	nocopy_put_req q = {.h = hash(k), .key = k, .klen = (uint32_t) klen, .koff = HDR, .size = HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(len)};
	nocopy_entry *e = nocopy_put_begin(p, c, &q);
	assert(e);
	return e;
}

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

static ar_slot slot_of(const nocopy_entry *e) { return ar_slot_make(ar_slot_tag(e->h), ar_off8(e)); }
static nocopy_entry *entry_of(ar_slot s) { return (nocopy_entry *) ar_at8(ar_slot_off8(s)); }

static bool found(nocopy_ctx *c, const char *k)
{
	nocopy_borrow_begin(c);
	bool f = nocopy_find(c, hash(k), k, (uint32_t) strlen(k)) != NULL;
	nocopy_borrow_end(c);
	return f;
}

/* Owner ids that cannot be had: no descriptor (a forked child at its fd limit), or every process slot held. Both: no worker slot. */
static void no_owner_id(void)
{
	pid_t p = fork();
	if (!p) {
		nocopy_ctx k;
		nocopy_ctx_init(&k);
		for (int fd; (fd = open("/dev/null", O_RDONLY)) >= 0;) {} /* every descriptor taken */
		_exit(!nocopy_after_fork(&k) && !ar_self && nocopy_ctx_slot(&k) < 0 ? 0 : 1);
	}
	int st;
	assert(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	p = fork();
	if (!p) {
		int fd = ar_region_reopen();
		for (uint32_t s = 0; s < NOCOPY_MAX_WORKERS; s++) { /* another open file holds them all (ours too: no conflict with itself) */
			struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = s, .l_len = 1};
			(void) fcntl(fd, F_OFD_SETLK, &fl);
		}
		nocopy_ctx k;
		nocopy_ctx_init(&k);
		_exit(fd >= 0 && !nocopy_after_fork(&k) && !ar_self ? 0 : 1);
	}
	assert(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* A free run that lies across the point a shard's search starts at (its home) is found. Every segment free. */
static void run_across_home(void)
{
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		ar_seg_owner[s] = s >= 8 && s < 12 ? 0 : NOCOPY_MAX_SHARDS + 1; /* someone else's, but 8..11 */
	}
	assert(ar_pool_take(1, 4, 10, 0) == 8);
	memset(ar_seg_owner, 0, ar_nsegs);
}

/* A forked child that got no worker slot holds none of its parent's pins: a slot it claims later starts without them
 * (kept as its own, never published, a segment would be read while it is reused). c: pins something. */
static void fork_without_slot(nocopy_ctx *c)
{
	static nocopy_ctx all[NOCOPY_MAX_WORKERS];
	int n = 0, up[2], down[2], st;
	bool pinned = false;
	for (uint32_t w = 0; w < ar_seg_words; w++) {
		pinned |= c->pins.mine[w] != 0;
	}
	assert(pinned);
	for (; n < NOCOPY_MAX_WORKERS; n++) {
		nocopy_ctx_init(&all[n]);
		if (!nocopy_claim(&all[n])) {
			break;
		}
	}
	assert(pipe(up) == 0 && pipe(down) == 0);
	pid_t p = fork();
	if (!p) {
		char b = 0;
		bool ok = !nocopy_after_fork(c) && nocopy_ctx_slot(c) < 0; /* every slot is the parent's */
		ok = ok && write(up[1], &b, 1) == 1 && read(down[0], &b, 1) == 1;
		ok = ok && nocopy_claim(c);
		for (uint32_t w = 0; ok && w < ar_seg_words; w++) {
			ok = !(c->pins.mine[w] & ~ar_pins_of(nocopy_ctx_slot(c))[w]);
		}
		_exit(ok ? 0 : 1);
	}
	char b;
	assert(read(up[0], &b, 1) == 1);
	nocopy_release(&all[0]);
	assert(write(down[1], &b, 1) == 1);
	assert(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	for (int i = 1; i < n; i++) {
		nocopy_release(&all[i]);
	}
	close(up[0]), close(up[1]), close(down[0]), close(down[1]);
}

/* A creator maps the arena at a random 2 MB-aligned base: two creations, two addresses (alike once in 2^22). */
static void random_base(void)
{
	uintptr_t b[2];
	for (int i = 0; i < 2; i++) {
		char name[48];
		nocopy_named nm;
		snprintf(name, sizeof name, "/nocopy-base-%d-%d", (int) getpid(), i);
		assert(nocopy_arena_open(&(nocopy_config) {.name = name, .size = 4 << 20, .abi = 1, .rebase = rebase}, &nm).code == NOCOPY_ERR_NONE);
		shm_unlink(nm.name);
		b[i] = (uintptr_t) ar_base;
		assert(b[i] % (2 << 20) == 0 && b[i] >= 0x1000000000ULL && b[i] < 0x400000000000ULL);
		nocopy_arena_close();
	}
	assert(b[0] != b[1]);
}

/* A huge entries_hint is refused, not an endless loop (hint * 2 overflowed) or taken as 0. */
static void huge_hint(void)
{
	ar_layout l;
	uint64_t hints[] = {(uint64_t) 1 << 34, (uint64_t) 1 << 63, INT64_MAX, UINT64_MAX};
	for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
		assert(ar_layout_plan(&l, 32 << 20, hints[i]).code == NOCOPY_ERR_TOO_SMALL);
	}
	assert(ar_layout_plan(&l, 32 << 20, 4096).code == NOCOPY_ERR_NONE);
}

static time_t clk = 1700000000;
static time_t fake(void) { return clk; }

/* An entry expiring this very second joins bucket 0's segment, whose expiry walk then dropped the never-expiring ones. */
static void expires_now(void)
{
	char name[48];
	nocopy_named nm;
	snprintf(name, sizeof name, "/nocopy-expnow-%d", (int) getpid());
	assert(nocopy_arena_open(&(nocopy_config) {.name = name, .size = 4 << 20, .abi = 1, .rebase = rebase}, &nm).code == NOCOPY_ERR_NONE);
	shm_unlink(nm.name);
	nocopy_set_clock(fake);
	nocopy_ctx c;
	nocopy_ctx_init(&c);
	assert(nocopy_claim(&c));
	const char *ks[] = {"forever", "edge", "later"};
	for (int i = 0; i < 3; i++) {
		nocopy_put_req q = {.h = hash(ks[i]), .key = ks[i], .klen = (uint32_t) strlen(ks[i]), .koff = HDR, .size = HDR + 64, .expires = i == 1 ? clk : 0};
		nocopy_put p;
		assert(nocopy_put_begin(&p, &c, &q) && nocopy_put_commit(&p, false) == NOCOPY_PUT_OK);
		clk += i == 1 ? 2 : 0;
	}
	assert(found(&c, "forever") && found(&c, "later") && !found(&c, "edge"));
	nocopy_set_clock(NULL);
	nocopy_release(&c);
	nocopy_arena_close();
}

int main(void)
{
	huge_hint();
	expires_now();
	random_base();
	char base[32];
	nocopy_named nm;
	snprintf(base, sizeof base, "/nocopy-repair-%d", (int) getpid());
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = 4 << 20, .hint = N, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name); /* this run's only: the mapping stays */
	assert(err.code == NOCOPY_ERR_NONE && ar_hdr->nshards == 1);
	nocopy_ctx c;
	nocopy_ctx_init(&c);
	assert(nocopy_claim(&c));
	ar_shard *sh = &ar_shards[0];
	ar_index *ix = &ar_ix[0];
	char k[32];
	nocopy_put p;

	for (int i = 0; i < N; i++) {
		snprintf(k, sizeof k, "k%d", i);
		begin(&p, &c, k, 100);
		assert(nocopy_put_commit(&p, false) == NOCOPY_PUT_OK);
	}
	/* a big allocation the holder never publishes: from here on we hold the shard lock */
	nocopy_entry *big = begin(&p, &c, "abandoned", ar_seg_size);
	/* a delete whose backward shift dies after its first move: mseq odd, the moved key in two slots, the victim still live */
	uint32_t m = ix->mask, i = 0, j = 0;
	for (;; i++) {
		assert(i < m); /* needs one displaced key */
		j = (i + 1) & m;
		if (ix->t[i] && ix->t[j] && ((j - (ar_slot_tag(entry_of(ix->t[j])->h) & m)) & m) >= 1) {
			break; /* t[j] is not at home: deleting t[i] moves it back */
		}
	}
	nocopy_entry *victim = entry_of(ix->t[i]), *moved = entry_of(ix->t[j]);
	char vk[32];
	snprintf(vk, sizeof vk, "%.*s", (int) victim->klen, nocopy_key(victim));
	ix->hdr->mseq++;
	ix->t[i] = ix->t[j];
	/* a segment claimed from the pool, never set up */
	uint32_t orphan = 0;
	while (ar_seg_owner[orphan]) {
		orphan++;
	}
	ar_seg_owner[orphan] = sh->log.tenant;
	sh->lock.holder = dead_owner(); /* the holder dies */
	ar_shards_forget_held();

	assert(!found(&c, "none")); /* a miss behind the odd mseq takes the lock: steals and repairs */
	assert(!nocopy_delete(hash("none"), "none", 4));
	assert(ar_proc->lock_steals == 1);
	assert(!(ix->hdr->mseq & 1));
	assert(ar_index_find(ix, slot_of(moved)) && !victim->live);
	uint32_t n = 0;
	size_t live = 0;
	for (uint32_t s = 0; s <= m; s++) {
		if (ix->t[s]) {
			nocopy_entry *e = entry_of(ix->t[s]);
			assert(ar_index_find(ix, slot_of(e)) == &ix->t[s]); /* no duplicates, every tag its entry's */
			n++;
			live += nocopy_entry_size(e);
		}
	}
	assert(n == N - 1 && ar_index_nlive(ix) == n && sh->log.live_bytes == live);
	assert(!ar_shards_verify()); /* the rest of the shard rebuilt too */
	assert(!ar_seg_owner[orphan] && !ar_log_owns(&sh->log, orphan) && sh->log.nown == (uint32_t) __builtin_popcountll(sh->log.own[0]));
	assert(!ar_segs[ar_seg_of(big)].pending); /* the dead holder's unpublished entry: dead bytes */
	for (int x = 0; x < N; x++) {
		snprintf(k, sizeof k, "k%d", x);
		assert(found(&c, k) == (strcmp(k, vk) != 0)); /* the victim's miss returns instead of retrying forever */
	}

	nocopy_drop_idle(&c);
	nocopy_clear(); /* seals the open segments: with nothing pinned, all go back to the pool */
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		assert(!ar_seg_owner[s] && ar_segs[s].state == SEG_FREE && !ar_log_owns(&sh->log, s) && !ar_segs[s].pending);
	}
	assert(sh->log.live_bytes == 0 && sh->log.nown == 0);
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		assert(sh->log.cur[b] == -1);
	}
	run_across_home();
	no_owner_id();
	nocopy_put hp;
	begin(&hp, &c, "held", 8);
	assert(nocopy_put_commit(&hp, false) == NOCOPY_PUT_OK && found(&c, "held"));
	fork_without_slot(&c);
	assert(!ar_shards_verify());
	printf("repair: ok (%u slots)\n", m + 1);
	return 0;
}
