/* nocopy arena: atoms, writer side. Admissions append to the current generation; when its half is full, a rotation wipes
 * the other half for the next generation once no writer's view holds it and no segment's entries depend on it (the pool's
 * deps bits, which writers set under their shard's lock: ar_put_req.deps). */

#ifndef NOCOPY_ARENA_ATOMS_H
#define NOCOPY_ARENA_ATOMS_H

#include "atoms_read.h"
#include "clock.h"
#include "proc.h"

/* One half of the area: one generation of atoms, append-only until a rotation wipes it for a later one. */
typedef struct {
	uint32_t state;   /* the generation it holds, | NOCOPY_ATOM_WIPING while a rotation checks or wipes it */
	uint32_t used, n; /* bytes appended, slots taken (atom lock) */
	uint32_t arrays, array_bytes; /* of those, arrays' */
} ar_atom_half;

/* In the arena header. Offsets are from ar_base. */
typedef struct {
	ar_lock lock __attribute__((aligned(64))); /* admissions and rotations */
	uint32_t gen;      /* the generation admissions go to; it lives in half gen & 1 */
	time_t tried;      /* last second a rotation found the other half still referenced */
	uint64_t rotations;
	ar_atom_half half[2];
	uint32_t half_bytes, slots; /* bytes per half, slots per half's table (a power of two) */
	size_t area_off, tab_off, dk_off;
} ar_atoms_hdr;

/* Both halves of arena_size / 32 (a warmed Drupal shares 0.3 MB of strings and 3.1 MB of arrays over 38 MB of entries), their tables
 * (a slot per 64 bytes, at most 3/4 used) and the doorkeeper, at `at`. Returns the end. */
size_t ar_atoms_plan(ar_atoms_hdr *h, size_t at, size_t arena_size);
void ar_atoms_init(ar_atoms_hdr *h); /* zero-filled memory, after ar_atoms_plan */
/* segments_use(bits): some segment's entries depend on those halves. Takes every shard's lock: deps change only under them. */
void ar_atoms_adopt(ar_atoms_hdr *h, bool (*segments_use)(uint8_t bits));

/* Announces the current generation in the worker slot (nested calls share the outer view); false: no worker slot, no atoms. */
bool ar_atom_begin(ar_atom_reader *r, int slot, nocopy_atom_view *v);
void ar_atom_end(ar_atom_reader *r, int slot); /* after the commit: the block's segment carries the bit by then */
void ar_atoms_reset_slot(int slot);            /* claimed from a dead owner, who may have died holding a view */

typedef struct {
	uint64_t atoms, used, size, rotations, arrays, array_bytes;
} ar_atoms_stats;
void ar_atoms_stat(ar_atoms_stats *s);

/* ---- API (nocopy_arena.h) ---- */

/* Stores s as an atom of the current generation (the blob: hlen bytes of hdr, s, a NUL; 8-aligned), rotating if its half is full;
 * a no-op if s is in already. Returns its blob if that is in v's generation (the writer may use it), else NULL; also NULL while another process admits. */
void *nocopy_atom_add(const nocopy_atom_view *v, uint64_t h, const void *hdr, uint32_t hlen, const char *s, uint32_t len);
/* An array as an atom of v's generation (never another: it may point at v's atoms): fill writes its size bytes at their final address,
 * the key (len | NOCOPY_ATOM_ARRAY) follows. Returns the blob, an equal key's if one is in, else NULL (full, busy, rotated). */
void *nocopy_atom_add_array(const nocopy_atom_view *v, uint64_t h, uint32_t size, void (*fill)(void *dst, void *arg), void *arg,
	const char *s, uint32_t len);

#endif
