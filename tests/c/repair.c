/* Shard repair: a lock taken from a dead holder rebuilds the shard. Builds the half-done states a kill can leave
 * (a delete's shift stopped midway, an unlinked entry still counted live, an abandoned allocation, a dead writer's
 * unfinished big entry, a segment claimed and never set up) and checks the shard afterwards. Inspects internals: layout.h. */
#include "../../arena/layout.h"
#include "../../arena/nocopy_arena.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
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

static uint64_t dead_owner(void) /* the id of a process that has exited and been reaped */
{
	pid_t p = fork();
	if (!p) {
		_exit(0);
	}
	waitpid(p, NULL, 0);
	return (uint64_t) (uint32_t) p | (uint64_t) 1 << 32;
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

int main(void)
{
	char base[32];
	nocopy_named nm;
	snprintf(base, sizeof base, "/nocopy-repair-%d", (int) getpid());
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = 4 << 20, .hint = N, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name); /* this run's only: the mapping stays */
	assert(err.code == NOCOPY_ERR_NONE && ar_hdr->nshards == 1);
	nocopy_ctx c, w;
	nocopy_ctx_init(&c);
	nocopy_ctx_init(&w);
	assert(nocopy_claim(&c) && nocopy_claim(&w));
	ar_shard *sh = &ar_shards[0];
	ar_index *ix = &ar_ix[0];
	char k[32];
	nocopy_put p;

	for (int i = 0; i < N; i++) {
		snprintf(k, sizeof k, "k%d", i);
		begin(&p, &c, k, 100);
		assert(nocopy_put_commit(&p, false) == NOCOPY_PUT_OK);
	}
	/* another writer's big entry, filled outside the lock; the writer dies */
	nocopy_entry *big = begin(&p, &w, "big", ar_seg_size);
	assert(!p.locked && ar_worker_at((uint32_t) nocopy_ctx_slot(&w))->wip);
	ar_worker_at((uint32_t) nocopy_ctx_slot(&w))->owner = dead_owner();
	/* an allocation the holder never publishes: from here on we hold the shard lock */
	begin(&p, &c, "abandoned", 300);
	assert(p.locked);
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

	assert(!nocopy_delete(hash("none"), "none", 4)); /* takes the lock: steals and repairs */
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
			live += e->size;
		}
	}
	assert(n == N - 1 && ar_index_nlive(ix) == n && sh->log.live_bytes == live);
	assert(!ar_seg_owner[orphan] && !ar_log_owns(&sh->log, orphan) && sh->log.nown == (uint32_t) __builtin_popcountll(sh->log.own[0]));
	assert(ar_segs[ar_seg_of(big)].pending == 1); /* its writer is not the dead holder: left for the reaper */
	for (int x = 0; x < N; x++) {
		snprintf(k, sizeof k, "k%d", x);
		assert(found(&c, k) == (strcmp(k, vk) != 0)); /* the victim's miss returns instead of retrying forever */
	}

	nocopy_drop_idle(&c);
	nocopy_clear(); /* reaps the dead writer's entry, seals the open segments: with nothing pinned, all go back to the pool */
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		assert(!ar_seg_owner[s] && ar_segs[s].state == SEG_FREE && !ar_log_owns(&sh->log, s) && !ar_segs[s].pending);
	}
	assert(!ar_worker_at((uint32_t) nocopy_ctx_slot(&w))->wip && sh->log.live_bytes == 0 && sh->log.nown == 0);
	for (int b = 0; b < NOCOPY_TTL_BUCKETS; b++) {
		assert(sh->log.cur[b] == -1);
	}
	printf("repair: ok (%u slots)\n", m + 1);
	return 0;
}
