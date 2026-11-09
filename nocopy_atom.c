/* nocopy arena: atoms, byte strings stored once for the whole arena (nocopy_arena.h). No PHP in here. */

#include "nocopy_arena_int.h"
#include <time.h>

char *nocopy_atoms;
nocopy_slot *nocopy_atom_tab;
uint32_t *nocopy_atom_dk;
uint32_t nocopy_atom_mask;
uint32_t nocopy_atom_bytes;

static nocopy_atom_half *half_of(uint32_t gen) { return &nocopy_a->atom[gen & 1]; }
static nocopy_slot *tab_of_gen(uint32_t gen) { return nocopy_atom_tab + (size_t) (gen & 1) * (nocopy_atom_mask + 1); }
static char *area_of(uint32_t gen) { return nocopy_atoms + (size_t) (gen & 1) * nocopy_atom_bytes; }
static uint32_t *agen_of(int slot) { return &nocopy_a->workers[slot].agen; }

/* Generation 2 in half 0; half 1 holds the empty generation 1, so the first rotation finds it unreferenced. Arena locks not needed: not yet shared. */
void nocopy_atom_init(nocopy_arena *p)
{
	p->atom_gen = 2;
	p->atom[0] = (nocopy_atom_half) {.state = 2, .used = 8}; /* offset 0 unused: a slot is never 0 */
	p->atom[1] = (nocopy_atom_half) {.state = 1, .used = 8};
}

bool nocopy_atom_begin(nocopy_ctx *c, nocopy_atom_view *v)
{
	if (c->slot < 0) {
		return false;
	}
	if (!c->anest) {
		/* Dekker with atom_rotate: announce, then check the half is not being wiped; a rotation sets WIPING, then reads announcements */
		for (;;) {
			uint32_t g = __atomic_load_n(&nocopy_a->atom_gen, __ATOMIC_ACQUIRE);
			__atomic_store_n(agen_of(c->slot), g, __ATOMIC_SEQ_CST);
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (__atomic_load_n(&half_of(g)->state, __ATOMIC_ACQUIRE) == g) {
				c->agen = g;
				break;
			}
			nocopy_cpu_relax(); /* g went stale and its half is being wiped: atom_gen has moved on, or will */
		}
	}
	c->anest++;
	v->gen = c->agen;
	return true;
}

void nocopy_atom_end(nocopy_ctx *c)
{
	if (!c->anest) {
		return;
	}
	c->anest--;
	if (!c->anest) {
		__atomic_store_n(agen_of(c->slot), 0, __ATOMIC_RELEASE);
	}
}

/* Atom lock: a trylock, since an admission can always be skipped. A dead holder's lock is taken over (checked at most once a second):
 * every state it can leave is consistent (nocopy_atom_add publishes in order, atom_rotate repeats from the start). */
static bool atom_trylock(void)
{
	static time_t checked;
	uint64_t z = 0;
	if (__atomic_compare_exchange_n(&nocopy_a->atom_lock, &z, nocopy_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return true;
	}
	time_t now = nocopy_now();
	if (z == nocopy_self || checked == now) { /* nocopy_self: another thread of this process */
		return false;
	}
	checked = now;
	if (!nocopy_owner_dead(z) || !__atomic_compare_exchange_n(&nocopy_a->atom_lock, &z, nocopy_self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return false;
	}
	__atomic_add_fetch(&nocopy_a->lock_steals, 1, __ATOMIC_RELAXED);
	return true;
}

static void atom_unlock(void) { __atomic_store_n(&nocopy_a->atom_lock, 0, __ATOMIC_RELEASE); }

/* Does a live writer's view hold old? Read after the half is marked WIPING; no lock held (owner_dead reads /proc). */
static bool viewed(uint32_t old)
{
	uint32_t nw = __atomic_load_n(&nocopy_a->nworkers, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < nw; i++) {
		if (__atomic_load_n(agen_of((int) i), __ATOMIC_SEQ_CST) != old) {
			continue;
		}
		uint64_t o = __atomic_load_n(&nocopy_a->workers[i].owner, __ATOMIC_RELAXED);
		if (o && (o == nocopy_self || !nocopy_owner_dead(o))) {
			return true;
		}
		__atomic_store_n(agen_of((int) i), 0, __ATOMIC_RELAXED); /* died mid-write */
	}
	return false;
}

/* Do a segment's entries use old? After viewed(): a view ends only after its put_begin set the bit under a shard lock,
 * and all of them are taken here, so every bit set by then is seen, and none changes meanwhile. */
static bool in_segments(uint32_t old)
{
	uint8_t bit = (uint8_t) (1u << (old & 1));
	bool used = false;
	for (uint32_t s = 0; s < nocopy_a->nshards; s++) { /* in index order, as nocopy_clear */
		nocopy_lock(&nocopy_a->shards[s]);
	}
	for (uint32_t s = 0; s < nocopy_a->nsegs && !used; s++) {
		used = nocopy_segs[s].atoms & bit;
	}
	for (uint32_t s = 0; s < nocopy_a->nshards; s++) {
		nocopy_unlock(&nocopy_a->shards[s]);
	}
	return used;
}

/* The current half is full: wipe the other one for the next generation, if nothing references its generation. Atom lock held. */
static bool atom_rotate(void)
{
	nocopy_arena *a = nocopy_a;
	time_t now = nocopy_now();
	if (__atomic_load_n(&a->atom_tried, __ATOMIC_RELAXED) == now) {
		return false;
	}
	uint32_t g = a->atom_gen, old = g - 1; /* the other half holds old, also when a dead rotation left it WIPING or half wiped */
	nocopy_atom_half *o = half_of(old);
	__atomic_store_n(&o->state, old | NOCOPY_ATOM_WIPING, __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (viewed(old) || in_segments(old)) {
		__atomic_store_n(&o->state, old, __ATOMIC_RELEASE);
		__atomic_store_n(&a->atom_tried, now, __ATOMIC_RELAXED);
		return false;
	}
	memset(tab_of_gen(old), 0, (size_t) a->atom_slots * sizeof(nocopy_slot));
	__atomic_store_n(&o->used, 8, __ATOMIC_RELAXED);
	__atomic_store_n(&o->n, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&o->state, g + 1, __ATOMIC_RELEASE);
	__atomic_store_n(&a->atom_gen, g + 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&a->atom_rotations, 1, __ATOMIC_RELAXED);
	return true;
}

void *nocopy_atom_add(const nocopy_atom_view *v, uint64_t h, const void *hdr, uint32_t hlen, const char *s, uint32_t len)
{
	if (__atomic_load_n(&nocopy_a->atom_tried, __ATOMIC_RELAXED) == nocopy_now() || !atom_trylock()) {
		return NULL; /* full and the other half still referenced this second, or another process admits */
	}
	char *blob = NULL;
	uint32_t need = 8 + (uint32_t) NOCOPY_ALIGN((size_t) hlen + len + 1), tag = nocopy_slot_tag(h), m = nocopy_atom_mask;
	for (int round = 0; round < 2; round++) {
		uint32_t g = nocopy_a->atom_gen;
		nocopy_atom_half *hf = half_of(g);
		nocopy_slot *t = tab_of_gen(g);
		char *area = area_of(g);
		uint32_t i = tag & m;
		for (; t[i]; i = (i + 1) & m) {
			const uint32_t *r = (const uint32_t *) (area + ((uint32_t) t[i] << 3));
			if ((uint32_t) (t[i] >> 32) == tag && r[0] == len && !memcmp((const char *) r + 8 + r[1], s, len)) {
				blob = (char *) r + 8; /* in already: another writer admitted it meanwhile */
				goto out;
			}
		}
		if (hf->n >= nocopy_a->atom_slots / 4 * 3 || need > nocopy_a->atom_half - hf->used) {
			if (round || !atom_rotate()) {
				goto out;
			}
			continue;
		}
		/* bytes, used, then the slot: a holder that dies anywhere leaves a state the next one can build on */
		uint32_t at = hf->used;
		char *rec = area + at;
		((uint32_t *) rec)[0] = len;
		((uint32_t *) rec)[1] = hlen;
		memcpy(rec + 8, hdr, hlen);
		memcpy(rec + 8 + hlen, s, len);
		rec[8 + hlen + len] = 0;
		__atomic_store_n(&hf->used, at + need, __ATOMIC_RELAXED); /* relaxed: nocopy_stats reads them racily */
		__atomic_store_n(&hf->n, hf->n + 1, __ATOMIC_RELAXED);
		__atomic_store_n(&t[i], (uint64_t) tag << 32 | (at >> 3), __ATOMIC_RELEASE);
		blob = rec + 8;
		break;
	}
out:;
	bool mine = nocopy_a->atom_gen == v->gen; /* a rotation in between: the blob is in a generation the view does not hold */
	atom_unlock();
	return mine ? blob : NULL;
}
