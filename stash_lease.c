/* stash arena: single-flight leases, a fixed table per shard under the shard lock. */

#include "stash_arena_int.h"

int stash_lease_take(uint64_t h, int32_t slot, time_t now, time_t ttl)
{
	stash_shard *sh = stash_shard_of(h);
	uint64_t dead = 0; /* holder found dead on the first pass: replace it if it still holds */
	for (;;) {
		stash_lock(sh);
		stash_lease *l = NULL, *free = NULL;
		for (int i = 0; i < STASH_LEASES; i++) {
			stash_lease *x = &sh->leases[i];
			if (x->owner && x->until >= now) {
				if (x->h == h) {
					l = x;
					break;
				}
			} else if (!free) {
				free = x;
			}
		}
		int r = STASH_LEASE_TAKEN;
		if (l && l->owner == stash_self && l->slot == slot) {
			r = STASH_LEASE_MINE;
		} else if (l && l->owner != dead) {
			r = STASH_LEASE_HELD;
		} else if (l || free) {
			*(l ? l : free) = (stash_lease) { .h = h, .owner = stash_self, .until = now + ttl, .slot = slot };
		} else {
			r = STASH_LEASE_FULL;
		}
		uint64_t o = r == STASH_LEASE_HELD ? l->owner : 0;
		stash_unlock(sh);
		if (r != STASH_LEASE_HELD || dead || !stash_owner_dead(o)) { /* reads /proc: not under the lock */
			return r;
		}
		dead = o;
	}
}

void stash_lease_drop(uint64_t h, int32_t slot)
{
	stash_shard *sh = stash_shard_of(h);
	stash_lock(sh);
	for (int i = 0; i < STASH_LEASES; i++) {
		stash_lease *x = &sh->leases[i];
		if (x->h == h && x->owner == stash_self && x->slot == slot) {
			x->owner = 0;
			break;
		}
	}
	stash_unlock(sh);
}
