/* shard_repair(): a lock taken from a dead holder rebuilds the shard. Builds the half-done states a kill can leave
 * (a delete's shift stopped midway, an unlinked entry still counted live, an abandoned allocation, a dead writer's
 * unfinished big entry) and checks the shard afterwards. Uses the arena's internal header. */
#include "../../nocopy_arena_int.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
#define N 400

void nocopy_rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) e, (void) from;
}

static uint64_t hash(const char *k)
{
	uint64_t h = 5381;
	while (*k) {
		h = h * 33 + (unsigned char) *k++;
	}
	return h | 1;
}

static nocopy_entry *build(nocopy_shard *sh, const char *k, size_t len) /* locked; allocated, not committed */
{
	size_t klen = strlen(k);
	nocopy_entry *e = nocopy_alloc(sh, HDR + NOCOPY_ALIGN(klen + 1) + NOCOPY_ALIGN(len), 0, time(NULL)); /* slots hold offset/8 */
	assert(e);
	e->h = hash(k);
	e->klen = (uint32_t) klen;
	e->koff = HDR;
	e->expires = 0;
	memcpy((char *) e + HDR, k, klen + 1);
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

static int bad_slots(nocopy_shard *sh) /* slots whose tag is not their entry's */
{
	nocopy_slot *t = tab_of(sh);
	int n = 0;
	for (uint32_t s = 0; s <= nocopy_tab_mask; s++) {
		n += t[s] && nocopy_slot_tag(nocopy_slot_entry(t[s])->h) != (uint32_t) (t[s] >> 32);
	}
	return n;
}

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
	const char *err = nocopy_arena_open(base, 4 << 20, N, 1, &(size_t){0}, &nm);
	shm_unlink(nm.name); /* this run's only: the mapping stays */
	assert(!err);
	nocopy_ctx c = {.slot = -1};
	assert(nocopy_claim(&c));
	nocopy_shard *sh = &nocopy_a->shards[0];
	char k[32];

	nocopy_lock(sh);
	for (int i = 0; i < N; i++) {
		snprintf(k, sizeof k, "k%d", i);
		assert(nocopy_commit(sh, build(sh, k, 100), false) == PUT_OK);
	}
	/* a delete whose backward shift dies after its first move: mseq odd, the moved key in two slots, the victim still live */
	nocopy_slot *t = tab_of(sh);
	uint32_t m = nocopy_tab_mask, i = 0, j = 0;
	for (;; i++) {
		assert(i < m); /* needs one displaced key */
		j = (i + 1) & m;
		if (t[i] && t[j] && (((j - ((uint32_t) (t[j] >> 32) & m)) & m) >= 1)) {
			break; /* t[j] is not at home: deleting t[i] moves it back */
		}
	}
	nocopy_entry *victim = nocopy_slot_entry(t[i]), *moved = nocopy_slot_entry(t[j]);
	char vk[32];
	strcpy(vk, nocopy_key(victim));
	sh->mseq++;
	t[i] = t[j];
	/* an allocation the holder never published */
	build(sh, "abandoned", 300);
	/* another dead writer's big entry, filled outside the lock */
	uint64_t other = dead_owner();
	nocopy_entry *big = build(sh, "big", nocopy_seg_size);
	nocopy_a->workers[1].owner = other;
	nocopy_a->workers[1].wip = (uint32_t) (((char *) big - (char *) nocopy_a) >> 3);
	if (nocopy_a->nworkers < 2) {
		nocopy_a->nworkers = 2;
	}
	/* a segment claimed from the pool, never set up */
	uint32_t orphan = 0;
	while (nocopy_seg_owner[orphan]) {
		orphan++;
	}
	nocopy_seg_owner[orphan] = (uint8_t) (sh->idx + 1);
	sh->lock = dead_owner(); /* the holder dies */

	nocopy_lock(sh); /* steals and repairs */
	assert(nocopy_a->lock_steals == 1);
	assert(!(sh->mseq & 1));
	assert(nocopy_slot_of(sh, moved) && !victim->live);
	uint32_t n = 0;
	size_t live = 0;
	for (uint32_t s = 0; s <= m; s++) {
		if (t[s]) {
			assert(nocopy_slot_of(sh, nocopy_slot_entry(t[s])) == &t[s]); /* no duplicates */
			n++;
			live += nocopy_slot_entry(t[s])->size;
		}
	}
	assert(n == N - 1 && sh->nlive == n && sh->live_bytes == live && !bad_slots(sh));
	assert(!nocopy_seg_owner[orphan] && !owns(sh, orphan) && sh->nown == (uint32_t) __builtin_popcountll(sh->own[0]));
	assert(nocopy_segs[nocopy_seg_of(big)].pending == 1); /* its writer is not the dead holder: left for the reaper */
	nocopy_unlock(sh);
	for (int x = 0; x < N; x++) {
		snprintf(k, sizeof k, "k%d", x);
		assert(found(&c, k) == (strcmp(k, vk) != 0)); /* the victim's miss returns instead of retrying forever */
	}

	nocopy_clear(); /* reaps the dead writer's entry; afterwards only open segments are left */
	uint32_t used = 0;
	for (uint32_t s = 0; s < nocopy_a->nsegs; s++) {
		uint8_t st = nocopy_segs[s].state;
		assert(nocopy_seg_owner[s] ? owns(sh, s) && st == SEG_OPEN : st == SEG_FREE && !owns(sh, s)); /* freed ones are back in the pool */
		used += st == SEG_OPEN;
		assert(!nocopy_segs[s].pending);
	}
	assert(!nocopy_a->workers[1].wip && sh->live_bytes == 0 && used <= NOCOPY_TTL_BUCKETS);
	printf("repair: ok (%u slots, %u open segments)\n", m + 1, used);
	return 0;
}
