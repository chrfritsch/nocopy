/* nocopy arena: atoms' reader side, byte strings stored once for the whole arena. The area has two halves, each holding one
 * generation; lookups search the current one. A record: uint32 len, uint32 offset of the bytes in the blob, the blob. Inline. */

#ifndef NOCOPY_ARENA_ATOMS_READ_H
#define NOCOPY_ARENA_ATOMS_READ_H

#include "index_read.h"

#define NOCOPY_ATOM_WIPING 0x80000000u
#define NOCOPY_ATOM_ARRAY 0x80000000u /* in a key's len: an array's record, never equal to a string's */
#define NOCOPY_ATOM_LEN 0x7fffffffu

/* A writer's view: the generation its lookups search, which no rotation wipes until the view ends. */
typedef struct {
	uint32_t gen;
} nocopy_atom_view;

/* Per process or thread: the generation announced, and the views begun and not yet ended (a nested write shares the outer view). */
typedef struct {
	uint32_t agen, anest;
} ar_atom_reader;

AR_HIDDEN extern char *ar_atoms;          /* both halves of the area */
AR_HIDDEN extern ar_slot *ar_atom_tab;    /* both halves' tables, ar_atom_mask + 1 slots each */
AR_HIDDEN extern uint32_t *ar_atom_dk;    /* doorkeeper: 2 * (ar_atom_mask + 1) tags */
AR_HIDDEN extern uint32_t ar_atom_mask;
AR_HIDDEN extern uint32_t ar_atom_bytes;  /* bytes per half: off the header line admissions write */

/* The bit of v's half: what a block using v's atoms depends on. */
static inline uint8_t nocopy_atom_bit(const nocopy_atom_view *v) { return (uint8_t) (1u << (v->gen & 1)); }

/* The half bit of an atom, 0 for anything outside the area. */
static inline uint8_t nocopy_atom_bits(const void *p)
{
	uintptr_t o = (uintptr_t) p - (uintptr_t) ar_atoms;
	return o < 2 * (uintptr_t) ar_atom_bytes ? (uint8_t) (1u << (o >= ar_atom_bytes)) : 0;
}

/* The blob of the atom equal to s in v's generation, or NULL. Lock-free: a slot is published after its record. */
static inline __attribute__((always_inline)) void *nocopy_atom_find(const nocopy_atom_view *v, uint64_t h, const char *s, uint32_t len)
{
	uint32_t half = v->gen & 1, m = ar_atom_mask, tag = ar_slot_tag(h);
	const ar_slot *t = ar_atom_tab + (size_t) half * (m + 1);
	char *area = ar_atoms + (size_t) half * ar_atom_bytes;
	for (uint32_t i = tag & m;; i = (i + 1) & m) {
		ar_slot sl = __atomic_load_n(&t[i], __ATOMIC_ACQUIRE);
		if (!sl) {
			return NULL;
		}
		if ((uint32_t) (sl >> 32) == tag) {
			char *rec = area + ((uint32_t) sl << 3);
			const uint32_t *r = (const uint32_t *) rec;
			if (r[0] == len && nocopy_key_eq(rec + 8 + r[1], s, len & NOCOPY_ATOM_LEN)) {
				return rec + 8;
			}
		}
	}
}

/* Doorkeeper (TinyLFU): true on the second sighting of h, so strings met once take no room. Direct-mapped, racy. */
static inline bool nocopy_atom_admit(uint64_t h)
{
	uint32_t *c = &ar_atom_dk[(uint32_t) ((h * 0x9E3779B97F4A7C15ULL) >> 32) & (2 * ar_atom_mask + 1)], t = (uint32_t) h | 1;
	if (__atomic_load_n(c, __ATOMIC_RELAXED) == t) {
		return true;
	}
	__atomic_store_n(c, t, __ATOMIC_RELAXED);
	return false;
}

#endif
