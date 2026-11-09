/* Atoms, deterministic: a rotation wipes a half only when nothing references its generation
 * (a segment's entries, a live writer's view), also after a dead holder left the atom lock or a rotation midway. Uses the internal header. */
#include "../../nocopy_arena_int.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))

uint8_t nocopy_rebase(nocopy_entry *e, const nocopy_entry *from)
{
	(void) e, (void) from;
	return 0;
}

static uint64_t hash(const char *k)
{
	uint64_t h = 5381;
	while (*k) {
		h = h * 33 + (unsigned char) *k++;
	}
	return h | 1;
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

static uint32_t next; /* words are never repeated: every add appends */
static nocopy_ctx adder = {.slot = -1}; /* its own worker slot: each add under a view of the current generation */

static const char *add(const char *w)
{
	nocopy_atom_view v;
	assert(nocopy_atom_begin(&adder, &v));
	const char *r = nocopy_atom_add(&v, hash(w), NULL, 0, w, (uint32_t) strlen(w));
	nocopy_atom_end(&adder);
	return r;
}

/* Adds new words until the current half is full; true if that rotated to the next generation. */
static bool fill(void)
{
	uint32_t g = nocopy_a->atom_gen;
	nocopy_a->atom_tried = 0; /* a refused rotation waits a second: not here */
	for (int i = 0; i < 100000 && nocopy_a->atom_gen == g; i++) {
		char w[48];
		snprintf(w, sizeof w, "w%u.....................................", next++);
		uint32_t n = nocopy_a->atom[g & 1].n;
		add(w);
		if (nocopy_a->atom_gen == g && nocopy_a->atom[g & 1].n == n) {
			return false; /* full, and the rotation was refused */
		}
	}
	return nocopy_a->atom_gen != g;
}

static const char *find(nocopy_ctx *c, const char *w)
{
	nocopy_atom_view v;
	assert(nocopy_atom_begin(c, &v));
	const char *r = nocopy_atom_find(&v, hash(w), w, (uint32_t) strlen(w));
	nocopy_atom_end(c);
	return r;
}

/* A committed entry that references the current generation (its segment carries the bit). */
static nocopy_entry *put_ref(nocopy_ctx *c, const char *k)
{
	nocopy_atom_view v;
	assert(nocopy_atom_begin(c, &v));
	size_t klen = strlen(k);
	nocopy_put p;
	nocopy_entry *e = nocopy_put_begin(&p, c, hash(k), (uint32_t) klen, HDR, HDR + NOCOPY_ALIGN(klen + 1), 0, nocopy_atom_bit(&v));
	assert(e);
	memcpy((char *) e + HDR, k, klen + 1);
	assert(nocopy_put_commit(&p, false) == PUT_OK);
	nocopy_atom_end(c);
	return e;
}

int main(void)
{
	char base[32];
	nocopy_named nm;
	snprintf(base, sizeof base, "/nocopy-atoms-%d", (int) getpid());
	const char *err = nocopy_arena_open(base, 4 << 20, 0, 1, &(size_t){0}, &nm);
	shm_unlink(nm.name);
	assert(!err);
	nocopy_ctx c = {.slot = -1};
	assert(nocopy_claim(&c) && nocopy_claim(&adder));
	assert(nocopy_a->atom_gen == 2);

	/* a view announces its generation in the worker slot; a nested one shares it, the outer one keeps it */
	nocopy_atom_view v, in;
	assert(nocopy_atom_begin(&c, &v) && v.gen == 2 && nocopy_a->workers[c.slot].agen == 2);
	const char *lang = add("languages:language_interface"); /* the adder may use it at once */
	assert(lang && nocopy_atom_find(&v, hash("languages:language_interface"), "languages:language_interface", 28) == lang);
	assert(nocopy_atom_begin(&c, &in) && in.gen == v.gen);
	nocopy_atom_end(&c);
	assert(nocopy_a->workers[c.slot].agen == v.gen);
	nocopy_atom_end(&c);
	assert(nocopy_a->workers[c.slot].agen == 0);
	assert(!strcmp(lang, "languages:language_interface"));
	assert(add("languages:language_interface") == lang && nocopy_a->atom[0].n == 1); /* in already: the same one */

	/* generation 2 full: the empty generation 1's half becomes generation 3 */
	nocopy_entry *e = put_ref(&c, "ref");
	assert(fill() && nocopy_a->atom_gen == 3 && nocopy_a->atom_rotations == 1);
	assert(!find(&c, "languages:language_interface")); /* only the current generation is searched */

	/* generation 3 full: generation 2 is still referenced by "ref"'s segment */
	assert(nocopy_segs[nocopy_seg_of(e)].atoms == 1);
	assert(!fill() && nocopy_a->atom_gen == 3);
	assert(nocopy_a->atom[0].state == 2 && !strcmp(lang, "languages:language_interface")); /* restored, untouched */

	/* deleted and cleared: the segment goes back to the pool with its bit, and the next fill rotates */
	assert(nocopy_delete(e->h, "ref", 3));
	nocopy_drop_idle(&c);
	nocopy_clear();
	assert(fill() && nocopy_a->atom_gen == 4);

	/* a writer that died holding a view of generation 3 does not block it */
	nocopy_ctx d = {.slot = -1};
	assert(nocopy_claim(&d));
	nocopy_a->workers[d.slot].agen = 3;
	nocopy_a->workers[d.slot].owner = dead_owner();
	assert(fill() && nocopy_a->atom_gen == 5 && nocopy_a->workers[d.slot].agen == 0);

	/* a live view of generation 5 does: until it ends */
	nocopy_atom_view held;
	assert(nocopy_atom_begin(&c, &held) && held.gen == 5);
	assert(fill() && nocopy_a->atom_gen == 6);
	assert(!fill() && nocopy_a->atom_gen == 6);
	nocopy_atom_end(&c);
	assert(fill() && nocopy_a->atom_gen == 7);

	/* a holder of the atom lock died midway through a rotation: the lock is taken over, the rotation done again */
	nocopy_a->atom_lock = dead_owner();
	nocopy_a->atom[0].state = 6 | NOCOPY_ATOM_WIPING;
	uint32_t steals = nocopy_a->lock_steals;
	assert(fill() && nocopy_a->atom_gen == 8 && nocopy_a->atom[0].state == 8 && nocopy_a->lock_steals == steals + 1);
	assert(nocopy_a->atom_lock == 0);

	nocopy_stats_t s;
	nocopy_stats(&s);
	assert(s.atom_rotations == 6 && s.atoms_size == 2 * (uint64_t) nocopy_a->atom_half);
	printf("atoms: ok (%u bytes a half, %u slots, %llu rotations)\n", nocopy_a->atom_half, nocopy_a->atom_slots, (unsigned long long) s.atom_rotations);
	return 0;
}
