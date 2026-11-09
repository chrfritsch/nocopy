/* nocopy arena: a process's (NTS) or thread's (ZTS) handle on the arena: its worker slot, pins and atom view. */

#ifndef NOCOPY_ARENA_CTX_H
#define NOCOPY_ARENA_CTX_H

#include "atoms_read.h"
#include "pool_read.h"

typedef struct nocopy_ctx {
	int pin_refs;        /* calls currently reading the arena */
	uint32_t rng;        /* xorshift state for the policy's read counter */
	ar_atom_reader atom;
	ar_pins_t pins;      /* pins.slot: the worker slot, -1 none */
} nocopy_ctx;

/* Before first use (no slot yet). */
static inline void nocopy_ctx_init(nocopy_ctx *c)
{
	memset(c, 0, sizeof *c);
	c->pins.slot = -1;
}

static inline int nocopy_ctx_slot(const nocopy_ctx *c) { return c->pins.slot; }

#endif
