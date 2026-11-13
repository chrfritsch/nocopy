/* nocopy arena: atoms: views, admission, rotation. */

#include "atoms.h"

char *ar_atoms;
ar_slot *ar_atom_tab;
uint32_t *ar_atom_dk;
uint32_t ar_atom_mask;
uint32_t ar_atom_bytes;

static ar_atoms_hdr *A;
static bool (*segments_use)(uint8_t bits);
static time_t lock_checked; /* last look at a holder of the atom lock that may be dead */

static ar_atom_half *half_of(uint32_t gen) { return &A->half[gen & 1]; }
static ar_slot *tab_of(uint32_t gen) { return ar_atom_tab + (size_t) (gen & 1) * (ar_atom_mask + 1); }
static char *area_of(uint32_t gen) { return ar_atoms + (size_t) (gen & 1) * ar_atom_bytes; }
static uint32_t *agen_of(int slot) { return &ar_worker_at((uint32_t) slot)->agen; }

size_t ar_atoms_plan(ar_atoms_hdr *h, size_t at, size_t arena_size)
{
	size_t a64 = ~(size_t) 63;
	h->half_bytes = (uint32_t) ((arena_size >> 5) & ~(size_t) 7);
	h->slots = 64;
	while (h->slots < h->half_bytes / 64) {
		h->slots <<= 1;
	}
	h->tab_off = (at + 63) & a64;
	h->dk_off = h->tab_off + 2 * (size_t) h->slots * sizeof(ar_slot);
	h->area_off = (h->dk_off + 2 * (size_t) h->slots * sizeof(uint32_t) + 63) & a64;
	return (h->area_off + 2 * (size_t) h->half_bytes + 63) & a64;
}

/* Generation 2 in half 0; half 1 holds the empty generation 1, so the first rotation finds it unreferenced. Not yet shared: no lock. */
void ar_atoms_init(ar_atoms_hdr *h)
{
	h->gen = 2;
	h->half[0] = (ar_atom_half) {.state = 2, .used = 8}; /* offset 0 unused: a slot is never 0 */
	h->half[1] = (ar_atom_half) {.state = 1, .used = 8};
}

void ar_atoms_adopt(ar_atoms_hdr *h, bool (*use)(uint8_t bits))
{
	A = h;
	segments_use = use;
	ar_atoms = ar_base + h->area_off;
	ar_atom_tab = (ar_slot *) (ar_base + h->tab_off);
	ar_atom_dk = (uint32_t *) (ar_base + h->dk_off);
	ar_atom_mask = h->slots - 1;
	ar_atom_bytes = h->half_bytes;
}

bool ar_atom_begin(ar_atom_reader *r, int slot, nocopy_atom_view *v)
{
	if (slot < 0) {
		return false;
	}
	if (!r->anest) {
		/* Dekker with rotate: announce, then check the half is not being wiped; a rotation sets WIPING, then reads announcements */
		for (;;) {
			uint32_t g = __atomic_load_n(&A->gen, __ATOMIC_ACQUIRE);
			AR_HOOK("view.loaded");
			__atomic_store_n(agen_of(slot), g, __ATOMIC_SEQ_CST);
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (__atomic_load_n(&half_of(g)->state, __ATOMIC_ACQUIRE) == g) {
				r->agen = g;
				break;
			}
			AR_HOOK("view.retry");
			ar_cpu_relax(); /* g went stale and its half is being wiped: gen has moved on, or will */
		}
	}
	r->anest++;
	v->gen = r->agen;
	return true;
}

void ar_atom_end(ar_atom_reader *r, int slot)
{
	if (!r->anest) {
		return;
	}
	r->anest--;
	if (!r->anest) {
		__atomic_store_n(agen_of(slot), 0, __ATOMIC_RELEASE);
	}
}

void ar_atoms_reset_slot(int slot) { __atomic_store_n(agen_of(slot), 0, __ATOMIC_RELAXED); }

/* A trylock: an admission can always be skipped. A stolen lock needs no repair: add publishes in order and rotate repeats from the start. */
static bool atom_lock(void) { return ar_lock_try(&A->lock, &lock_checked, nocopy_now()) != AR_TRY_BUSY; }
static void atom_unlock(void) { ar_lock_release(&A->lock); }

/* Does a live writer's view hold old? Read after the half is marked WIPING; no shard lock held (a liveness check reads /proc). */
static bool viewed(uint32_t old)
{
	uint32_t nw = ar_worker_high();
	for (uint32_t i = 0; i < nw; i++) {
		if (__atomic_load_n(agen_of((int) i), __ATOMIC_SEQ_CST) != old) {
			continue;
		}
		if (ar_worker_live(i)) {
			return true;
		}
		AR_HOOK("view.dead"); /* died mid-write: ignored, not cleared (the slot may have a new owner by now, who resets it) */
	}
	return false;
}

/* The current half is full: wipe the other one for the next generation, if nothing references its generation. Atom lock held.
 * segments_use after viewed: a view ends only after its put set the deps bit under a shard lock, and segments_use takes them all,
 * so every bit set by then is seen, and none changes meanwhile. */
static bool rotate(void)
{
	time_t now = nocopy_now();
	if (__atomic_load_n(&A->tried, __ATOMIC_RELAXED) == now) {
		return false;
	}
	uint32_t g = A->gen, old = g - 1; /* the other half holds old, also when a dead rotation left it WIPING or half wiped */
	ar_atom_half *o = half_of(old);
	__atomic_store_n(&o->state, old | NOCOPY_ATOM_WIPING, __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	AR_HOOK("rotate.marked");
	if (viewed(old) || segments_use((uint8_t) (1u << (old & 1)))) {
		__atomic_store_n(&o->state, old, __ATOMIC_RELEASE);
		__atomic_store_n(&A->tried, now, __ATOMIC_RELAXED);
		return false;
	}
	AR_HOOK("rotate.unused");
	memset(tab_of(old), 0, (size_t) A->slots * sizeof(ar_slot));
	AR_HOOK("rotate.wiped");
	__atomic_store_n(&o->used, 8, __ATOMIC_RELAXED);
	__atomic_store_n(&o->n, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&o->arrays, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&o->array_bytes, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&o->state, g + 1, __ATOMIC_RELEASE);
	AR_HOOK("rotate.state");
	__atomic_store_n(&A->gen, g + 1, __ATOMIC_RELEASE);
	AR_HOOK("rotate.gen");
	__atomic_add_fetch(&A->rotations, 1, __ATOMIC_RELAXED);
	return true;
}

static bool copy_fill(void *dst, void *arg)
{
	memcpy(dst, ((const void **) arg)[0], (size_t) (uintptr_t) ((const void **) arg)[1]);
	return true;
}

/* hlen bytes by fill, then the key: len's flag kept in the record, the bytes compared without it. */
static void *add(const nocopy_atom_view *v, uint64_t h, uint32_t hlen, bool (*fill)(void *, void *), void *arg, const char *s, uint32_t len)
{
	if (__atomic_load_n(&A->tried, __ATOMIC_RELAXED) == nocopy_now() || !atom_lock()) {
		return NULL; /* full and the other half still referenced this second, or another process admits */
	}
	bool array = len & NOCOPY_ATOM_ARRAY;
	uint32_t klen = len & NOCOPY_ATOM_LEN;
	char *blob = NULL;
	uint32_t need = 8 + (uint32_t) NOCOPY_ALIGN((size_t) hlen + klen + 1), tag = ar_slot_tag(h), m = ar_atom_mask;
	for (int round = 0; round < 2; round++) {
		uint32_t g = A->gen;
		if (array && g != v->gen) {
			goto out; /* rotated: the array may point at the view's atoms, so only the view's half may hold it */
		}
		ar_atom_half *hf = half_of(g);
		ar_slot *t = tab_of(g);
		char *area = area_of(g);
		uint32_t i = tag & m;
		for (; t[i]; i = (i + 1) & m) {
			const uint32_t *r = (const uint32_t *) (area + ((uint32_t) t[i] << 3));
			if ((uint32_t) (t[i] >> 32) == tag && r[0] == len && !memcmp((const char *) r + 8 + r[1], s, klen)) {
				blob = (char *) r + 8; /* in already: another writer admitted it meanwhile */
				goto out;
			}
		}
		if (hf->n >= A->slots / 4 * 3 || need > A->half_bytes - hf->used) {
			if (round || !rotate()) {
				goto out;
			}
			continue;
		}
		/* bytes, used, then the slot: a holder that dies anywhere leaves a state the next one can build on */
		uint32_t at = hf->used;
		char *rec = area + at;
		((uint32_t *) rec)[0] = len;
		((uint32_t *) rec)[1] = hlen;
		if (!fill(rec + 8, arg)) {
			goto out; /* nothing published: used and the slot are untouched */
		}
		memcpy(rec + 8 + hlen, s, klen);
		rec[8 + hlen + klen] = 0;
		AR_HOOK("atom.bytes");
		__atomic_store_n(&hf->used, at + need, __ATOMIC_RELAXED); /* relaxed: stats read them racily */
		__atomic_store_n(&hf->n, hf->n + 1, __ATOMIC_RELAXED);
		if (array) {
			__atomic_store_n(&hf->arrays, hf->arrays + 1, __ATOMIC_RELAXED);
			__atomic_store_n(&hf->array_bytes, hf->array_bytes + need, __ATOMIC_RELAXED);
		}
		AR_HOOK("atom.used");
		__atomic_store_n(&t[i], ar_slot_make(tag, at >> 3), __ATOMIC_RELEASE);
		blob = rec + 8;
		break;
	}
out:;
	bool mine = A->gen == v->gen; /* a rotation in between: the blob is in a generation the view does not hold */
	atom_unlock();
	return mine ? blob : NULL;
}

void *nocopy_atom_add(const nocopy_atom_view *v, uint64_t h, const void *hdr, uint32_t hlen, const char *s, uint32_t len)
{
	const void *arg[2] = {hdr, (const void *) (uintptr_t) hlen};
	return add(v, h, hlen, copy_fill, arg, s, len);
}

void *nocopy_atom_add_array(const nocopy_atom_view *v, uint64_t h, uint32_t size, bool (*fill)(void *dst, void *arg), void *arg,
	const char *s, uint32_t len)
{
	return add(v, h, size, fill, arg, s, len | NOCOPY_ATOM_ARRAY);
}

void ar_atoms_stat(ar_atoms_stats *s)
{
	const ar_atom_half *cur = half_of(__atomic_load_n(&A->gen, __ATOMIC_RELAXED));
	s->atoms = __atomic_load_n(&cur->n, __ATOMIC_RELAXED);
	s->used = __atomic_load_n(&A->half[0].used, __ATOMIC_RELAXED) + __atomic_load_n(&A->half[1].used, __ATOMIC_RELAXED) - 16;
	s->size = 2 * (uint64_t) A->half_bytes;
	s->rotations = __atomic_load_n(&A->rotations, __ATOMIC_RELAXED);
	s->arrays = __atomic_load_n(&cur->arrays, __ATOMIC_RELAXED);
	s->array_bytes = __atomic_load_n(&A->half[0].array_bytes, __ATOMIC_RELAXED) + __atomic_load_n(&A->half[1].array_bytes, __ATOMIC_RELAXED);
}
