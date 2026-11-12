/* Atoms, deterministic: a rotation wipes a half only when nothing references its generation
 * (a segment's entries, a live writer's view), also after a dead holder left the atom lock or a rotation midway. Inspects internals: layout.h. */
#include "../../arena/layout.h"
#include "../../arena/nocopy_arena.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define HDR NOCOPY_ALIGN(sizeof(nocopy_entry))
#define AT (&ar_hdr->atoms)

static uint8_t rebase(nocopy_entry *e, const nocopy_entry *from)
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

static uint64_t dead_owner(void) /* the id of a process that has exited and been reaped: alive until then */
{
	int up[2], down[2];
	assert(pipe(up) == 0 && pipe(down) == 0);
	pid_t p = fork();
	if (!p) {
		char c;
		close(down[1]);
		ar_proc_attach(ar_region_reopen());
		if (write(up[1], &ar_self, sizeof ar_self) == sizeof ar_self) {
			(void) !read(down[0], &c, 1);
		}
		_exit(0);
	}
	uint64_t id = 0;
	assert(read(up[0], &id, sizeof id) == sizeof id && id && id != ar_self && !ar_owner_dead(id));
	close(down[1]);
	waitpid(p, NULL, 0);
	assert(ar_owner_dead(id));
	close(up[0]);
	close(up[1]);
	close(down[0]);
	return id;
}

static uint32_t next; /* words are never repeated: every add appends */
static nocopy_ctx adder; /* its own worker slot: each add under a view of the current generation */

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
	uint32_t g = AT->gen;
	AT->tried = 0; /* a refused rotation waits a second: not here */
	for (int i = 0; i < 100000 && AT->gen == g; i++) {
		char w[48];
		snprintf(w, sizeof w, "w%u.....................................", next++);
		uint32_t n = AT->half[g & 1].n;
		add(w);
		if (AT->gen == g && AT->half[g & 1].n == n) {
			return false; /* full, and the rotation was refused */
		}
	}
	return AT->gen != g;
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
	nocopy_put_req q = {.h = hash(k), .key = k, .klen = (uint32_t) klen, .koff = HDR, .size = HDR + NOCOPY_ALIGN(klen + 1), .deps = nocopy_atom_bit(&v)};
	nocopy_entry *e = nocopy_put_begin(&p, c, &q);
	assert(e);
	assert(nocopy_put_commit(&p, false) == NOCOPY_PUT_OK);
	nocopy_atom_end(c);
	return e;
}

static void check(void)
{
	const char *why = nocopy_arena_check();
	if (why) {
		fprintf(stderr, "atoms: arena: %s\n", why);
		exit(1);
	}
}

int main(void)
{
	char base[32];
	nocopy_named nm;
	snprintf(base, sizeof base, "/nocopy-atoms-%d", (int) getpid());
	nocopy_err err = nocopy_arena_open(&(nocopy_config) {.name = base, .size = 4 << 20, .abi = 1, .rebase = rebase}, &nm);
	shm_unlink(nm.name);
	assert(err.code == NOCOPY_ERR_NONE);
	nocopy_ctx c;
	nocopy_ctx_init(&c);
	nocopy_ctx_init(&adder);
	assert(nocopy_claim(&c) && nocopy_claim(&adder));
	assert(AT->gen == 2);

	/* a view announces its generation in the worker slot; a nested one shares it, the outer one keeps it */
	nocopy_atom_view v, in;
	assert(nocopy_atom_begin(&c, &v) && v.gen == 2 && ar_worker_at((uint32_t) nocopy_ctx_slot(&c))->agen == 2);
	const char *lang = add("languages:language_interface"); /* the adder may use it at once */
	assert(lang && nocopy_atom_find(&v, hash("languages:language_interface"), "languages:language_interface", 28) == lang);
	assert(nocopy_atom_begin(&c, &in) && in.gen == v.gen);
	nocopy_atom_end(&c);
	assert(ar_worker_at((uint32_t) nocopy_ctx_slot(&c))->agen == v.gen);
	nocopy_atom_end(&c);
	assert(ar_worker_at((uint32_t) nocopy_ctx_slot(&c))->agen == 0);
	assert(!strcmp(lang, "languages:language_interface"));
	assert(add("languages:language_interface") == lang && AT->half[0].n == 1); /* in already: the same one */

	/* generation 2 full: the empty generation 1's half becomes generation 3 */
	nocopy_entry *e = put_ref(&c, "ref");
	assert(fill() && AT->gen == 3 && AT->rotations == 1);
	assert(!find(&c, "languages:language_interface")); /* only the current generation is searched */

	/* generation 3 full: generation 2 is still referenced by "ref"'s segment */
	assert(ar_seg_deps(ar_seg_of(e)) == 1);
	assert(!fill() && AT->gen == 3);
	check();
	assert(AT->half[0].state == 2 && !strcmp(lang, "languages:language_interface")); /* restored, untouched */

	/* deleted and cleared: the segment goes back to the pool with its bit, and the next fill rotates */
	assert(nocopy_delete(hash("ref"), "ref", 3));
	nocopy_drop_idle(&c);
	nocopy_clear();
	assert(fill() && AT->gen == 4);
	check();

	/* a writer that died holding a view of generation 3 does not block it */
	nocopy_ctx d;
	nocopy_ctx_init(&d);
	assert(nocopy_claim(&d));
	ar_worker_at((uint32_t) nocopy_ctx_slot(&d))->agen = 3;
	ar_worker_at((uint32_t) nocopy_ctx_slot(&d))->owner = dead_owner();
	assert(fill() && AT->gen == 5 /* the dead worker's announcement is ignored, not written: its slot's next owner resets it (crash.c) */);

	/* a live view of generation 5 does: until it ends */
	nocopy_atom_view held;
	assert(nocopy_atom_begin(&c, &held) && held.gen == 5);
	assert(fill() && AT->gen == 6);
	assert(!fill() && AT->gen == 6);
	nocopy_atom_end(&c);
	assert(fill() && AT->gen == 7);
	check();

	/* a holder of the atom lock died midway through a rotation: the lock is taken over, the rotation done again */
	AT->lock.holder = dead_owner();
	AT->half[0].state = 6 | NOCOPY_ATOM_WIPING;
	uint32_t steals = ar_proc->lock_steals;
	assert(fill() && AT->gen == 8 && AT->half[0].state == 8 && ar_proc->lock_steals == steals + 1);
	assert(AT->lock.holder == 0);
	check();

	assert(!ar_shards_verify());
	nocopy_stats_t s;
	nocopy_stats(&s);
	assert(s.atom_rotations == 6 && s.atoms_size == 2 * (uint64_t) AT->half_bytes);
	printf("atoms: ok (%u bytes a half, %u slots, %llu rotations)\n", AT->half_bytes, AT->slots, (unsigned long long) s.atom_rotations);
	return 0;
}
