/* nocopy arena: single-flight leases, a fixed table per shard under the shard lock. */

#include "nocopy_arena_int.h"

int nocopy_lease_take(uint64_t h, int32_t slot, time_t now, time_t ttl)
{
	nocopy_shard *sh = nocopy_shard_of(h);
	uint64_t dead = 0; /* holder found dead on the first pass: replace it if it still holds */
	for (;;) {
		nocopy_lock(sh);
		nocopy_lease *l = NULL, *free = NULL;
		for (int i = 0; i < NOCOPY_LEASES; i++) {
			nocopy_lease *x = &sh->leases[i];
			if (x->owner && x->until >= now) {
				if (x->h == h) {
					l = x;
					break;
				}
			} else if (!free) {
				free = x;
			}
		}
		int r = NOCOPY_LEASE_TAKEN;
		if (l && l->owner == nocopy_self && l->slot == slot) {
			r = NOCOPY_LEASE_MINE;
		} else if (l && l->owner != dead) {
			r = NOCOPY_LEASE_HELD;
		} else if (l || free) {
			*(l ? l : free) = (nocopy_lease) { .h = h, .owner = nocopy_self, .until = now + ttl, .slot = slot };
		} else {
			r = NOCOPY_LEASE_FULL;
		}
		uint64_t o = r == NOCOPY_LEASE_HELD ? l->owner : 0;
		nocopy_unlock(sh);
		if (r != NOCOPY_LEASE_HELD || dead || !nocopy_owner_dead(o)) { /* reads /proc: not under the lock */
			return r;
		}
		dead = o;
	}
}

void nocopy_lease_drop(uint64_t h, int32_t slot)
{
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	for (int i = 0; i < NOCOPY_LEASES; i++) {
		nocopy_lease *x = &sh->leases[i];
		if (x->h == h && x->owner == nocopy_self && x->slot == slot) {
			x->owner = 0;
			break;
		}
	}
	nocopy_unlock(sh);
}
