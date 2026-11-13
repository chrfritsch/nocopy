/* nocopy arena: single-flight leases. A stolen table lock needs no repair: a lease changes by single stores. */

#include "leases.h"

static ar_lease_table *tables;
static uint32_t mask;

void ar_leases_adopt(ar_lease_table *t, uint32_t ntables)
{
	tables = t;
	mask = ntables - 1;
}

static ar_lease_table *table_of(uint64_t h) { return &tables[ar_part(h, mask)]; }

static void lock(ar_lease_table *t)
{
	uint64_t dead;
	(void) ar_lock_acquire(&t->lock, &dead);
}

typedef struct {
	ar_lease *held; /* h's unexpired lease, or NULL */
	ar_lease *free; /* the first free lease before it, or NULL */
} lease_search;

static lease_search find_lease(ar_lease_table *t, uint64_t h, time_t now)
{
	lease_search found = {NULL, NULL};
	for (int i = 0; i < NOCOPY_LEASES; i++) {
		ar_lease *x = &t->l[i];
		if (x->owner && x->until >= now) {
			if (x->h == h) {
				found.held = x;
				return found;
			}
		} else if (!found.free) {
			found.free = x;
		}
	}
	return found;
}

int nocopy_lease_take(uint64_t h, int32_t slot, time_t now, time_t ttl)
{
	ar_lease_table *t = table_of(h);
	uint64_t dead = 0; /* holder found dead on the first pass: replace it if it still holds */
	for (;;) {
		lock(t);
		lease_search found = find_lease(t, h, now);
		ar_lease *l = found.held, *free = found.free;
		int r = NOCOPY_LEASE_TAKEN;
		if (l && l->owner == ar_self && l->slot == slot) {
			r = NOCOPY_LEASE_MINE;
		} else if (l && l->owner != dead) {
			r = NOCOPY_LEASE_HELD;
		} else if (l || free) {
			*(l ? l : free) = (ar_lease) {.h = h, .owner = ar_self, .until = now + ttl, .slot = slot};
			AR_HOOK("lease.taken");
		} else {
			r = NOCOPY_LEASE_FULL;
		}
		uint64_t o = r == NOCOPY_LEASE_HELD ? l->owner : 0;
		ar_lock_release(&t->lock);
		if (r != NOCOPY_LEASE_HELD || dead || !ar_owner_dead(o)) { /* a syscall: not under the lock */
			return r;
		}
		dead = o;
		AR_HOOK("lease.dead");
	}
}

void nocopy_lease_drop(uint64_t h, int32_t slot)
{
	ar_lease_table *t = table_of(h);
	lock(t);
	for (int i = 0; i < NOCOPY_LEASES; i++) {
		ar_lease *x = &t->l[i];
		if (x->h == h && x->owner == ar_self && x->slot == slot) {
			x->owner = 0;
			break;
		}
	}
	ar_lock_release(&t->lock);
}

uint64_t ar_leases_held(time_t now)
{
	uint64_t n = 0;
	for (uint32_t i = 0; i <= mask; i++) {
		for (int j = 0; j < NOCOPY_LEASES; j++) {
			n += __atomic_load_n(&tables[i].l[j].owner, __ATOMIC_RELAXED) && tables[i].l[j].until >= now;
		}
	}
	return n;
}
