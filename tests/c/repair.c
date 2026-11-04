/* shard_repair(): a lock taken from a dead holder rebuilds the shard. Builds the half-done states a kill can leave
 * (a delete's shift stopped midway, an unlinked entry still counted live, an abandoned allocation, a dead writer's
 * unfinished big entry) and checks the shard afterwards. Uses the arena's internal header. */
#include "../../stash_arena_int.h"
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define HDR STASH_ALIGN(sizeof(stash_entry))
#define N 400

void stash_rebase(stash_entry *e, const stash_entry *from)
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

static stash_entry *build(stash_shard *sh, const char *k, size_t len) /* locked; allocated, not committed */
{
	size_t klen = strlen(k);
	stash_entry *e = stash_alloc(sh, HDR + STASH_ALIGN(klen + 1) + STASH_ALIGN(len), 0, time(NULL)); /* slots hold offset/8 */
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

static int bad_slots(stash_shard *sh) /* slots whose tag is not their entry's hash */
{
	stash_slot *t = tab_of(sh);
	int n = 0;
	for (uint32_t s = 0; s <= stash_tab_mask; s++) {
		n += t[s] && (uint32_t) stash_slot_entry(t[s])->h != (uint32_t) (t[s] >> 32);
	}
	return n;
}

static bool found(stash_ctx *c, const char *k)
{
	stash_borrow_begin(c);
	bool f = stash_find(c, hash(k), k, (uint32_t) strlen(k)) != NULL;
	stash_borrow_end(c);
	return f;
}

int main(void)
{
	assert(!stash_arena_create(4 << 20, N, &(size_t){0}));
	stash_ctx c = {.slot = -1};
	assert(stash_claim(&c));
	stash_shard *sh = &stash_a->shards[0];
	char k[32];

	stash_lock(sh);
	for (int i = 0; i < N; i++) {
		snprintf(k, sizeof k, "k%d", i);
		assert(stash_commit(sh, build(sh, k, 100), false) == PUT_OK);
	}
	/* a delete whose backward shift dies after its first move: mseq odd, the moved key in two slots, the victim still live */
	stash_slot *t = tab_of(sh);
	uint32_t m = stash_tab_mask, i = 0, j = 0;
	for (;; i++) {
		j = (i + 1) & m;
		if (t[i] && t[j] && (((j - ((uint32_t) (t[j] >> 32) & m)) & m) >= 1)) {
			break; /* t[j] is not at home: deleting t[i] moves it back */
		}
	}
	stash_entry *victim = stash_slot_entry(t[i]), *moved = stash_slot_entry(t[j]);
	char vk[32];
	strcpy(vk, stash_key(victim));
	sh->mseq++;
	t[i] = t[j];
	/* an allocation the holder never published */
	build(sh, "abandoned", 300);
	/* another dead writer's big entry, filled outside the lock */
	uint64_t other = dead_owner();
	stash_entry *big = build(sh, "big", stash_seg_size);
	stash_a->workers[1].owner = other;
	stash_a->workers[1].wip = (uint32_t) (((char *) big - (char *) stash_a) >> 3);
	if (stash_a->nworkers < 2) {
		stash_a->nworkers = 2;
	}
	sh->lock = dead_owner(); /* the holder dies */

	stash_lock(sh); /* steals and repairs */
	assert(stash_a->lock_steals == 1);
	assert(!(sh->mseq & 1));
	assert(stash_slot_of(sh, moved) && !victim->live);
	uint32_t n = 0;
	size_t live = 0;
	for (uint32_t s = 0; s <= m; s++) {
		if (t[s]) {
			assert(stash_slot_of(sh, stash_slot_entry(t[s])) == &t[s]); /* no duplicates */
			n++;
			live += stash_slot_entry(t[s])->size;
		}
	}
	assert(n == N - 1 && sh->nlive == n && sh->live_bytes == live && !bad_slots(sh));
	assert(stash_a->segs[stash_seg_of(big)].pending == 1); /* its writer is not the dead holder: left for the reaper */
	stash_unlock(sh);
	for (int x = 0; x < N; x++) {
		snprintf(k, sizeof k, "k%d", x);
		assert(found(&c, k) == (strcmp(k, vk) != 0)); /* the victim's miss returns instead of retrying forever */
	}

	stash_clear(); /* reaps the dead writer's entry; afterwards only open segments are left */
	uint32_t used = 0;
	for (uint32_t s = sh->lo; s < sh->hi; s++) {
		uint8_t st = stash_a->segs[s].state;
		assert(st == SEG_FREE || st == SEG_OPEN);
		used += st == SEG_OPEN;
		assert(!stash_a->segs[s].pending);
	}
	assert(!stash_a->workers[1].wip && sh->live_bytes == 0 && used <= STASH_TTL_BUCKETS);
	printf("repair: ok (%u slots, %u open segments)\n", m + 1, used);
	return 0;
}
