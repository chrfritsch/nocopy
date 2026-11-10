/* nocopy arena: the shared-memory region. One POSIX shm object per name and build, mapped at the same address in every
 * process that attaches (pointers are valid everywhere). Knows nothing of what is laid out in it beyond its own header. */

#ifndef NOCOPY_ARENA_REGION_H
#define NOCOPY_ARENA_REGION_H

#include "base.h"

/* At offset 0 of every region. magic is stored last, once the creator's init has run. */
typedef struct {
	uint64_t magic, abi;
	uintptr_t base; /* the address every process maps it at */
	size_t size;
} ar_region_hdr;

typedef struct {
	char name[64];  /* the POSIX shm object: base.<abi> */
	char other[64]; /* another build's region under the same base, still in use by its processes; "" none */
} nocopy_named;

/* Creates or attaches to base.<abi>, mapped at the same address in every process. A creator maps size bytes, zero-filled,
 * and runs init(mem, arg) before publishing it; an attacher gets the creator's size. Unused regions of other builds under
 * base are removed (Linux). On success *mem is the mapping, held (a shared flock) until ar_region_close. */
nocopy_err ar_region_open(const char *base, uint64_t abi, size_t size, void (*init)(void *mem, void *arg), void *arg, nocopy_named *out, void **mem);
void ar_region_unhold(void);  /* map without counting as a user (the Valkey listener) */
bool ar_region_current(void); /* false: its name was removed or names another object now */
int ar_region_reopen(void);   /* a new descriptor of the object (its own open file), -1 if none */
void ar_region_close(void *mem);

#endif
