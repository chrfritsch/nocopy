/* nocopy arena: the API's own calls: opening, worker slots, iteration, stats. */

#include "layout.h"
#include "nocopy_arena.h"

char *ar_user;
nocopy_clock_fn ar_clock_fake;

typedef struct {
	const ar_layout *l;
} creating;

static void init(void *mem, void *arg) { ar_layout_init(mem, ((creating *) arg)->l); }

nocopy_err nocopy_arena_open(const nocopy_config *cfg, nocopy_named *out)
{
	ar_layout l;
	memset(out, 0, sizeof *out);
	nocopy_err err = ar_layout_plan(&l, cfg->size, cfg->hint);
	if (err.code != NOCOPY_ERR_NONE) {
		return err;
	}
	void *mem;
	creating c = {.l = &l};
	err = ar_region_open(cfg->name, cfg->abi, l.size, init, &c, out, &mem);
	if (err.code != NOCOPY_ERR_NONE) {
		return err;
	}
	if (!ar_layout_adopt(mem, cfg->rebase)) {
		ar_region_close(mem);
		return (nocopy_err) {.code = NOCOPY_ERR_FAILED, .msg = "its header is corrupt"};
	}
	ar_user = ar_hdr->user;
	return err;
}

void nocopy_arena_unhold(void) { ar_region_unhold(); }
bool nocopy_arena_current(void) { return ar_region_current(); }

void nocopy_arena_close(void)
{
	ar_region_close(ar_hdr);
	ar_hdr = NULL;
	ar_base = NULL;
	ar_size = 0;
	ar_user = NULL;
}

void nocopy_set_clock(nocopy_clock_fn fn) { ar_clock_fake = fn; }

/* ---- worker slots ---- */

bool nocopy_claim(nocopy_ctx *c)
{
	int i = ar_worker_claim();
	c->pins.slot = i;
	if (i < 0) {
		return false;
	}
	ar_pins_clear(i);
	ar_shard_wip_abort(i); /* its previous owner died filling an entry */
	ar_atoms_reset_slot(i); /* ... or holding an atom view */
	c->rng = ((uint32_t) (ar_self ^ (ar_self >> 32)) * 2654435761u + (uint32_t) i) | 1;
	return true;
}

void nocopy_release(nocopy_ctx *c)
{
	if (c->pins.slot >= 0) {
		ar_pins_clear(c->pins.slot);
		ar_worker_release(c->pins.slot);
		c->pins.slot = -1;
	}
}

/* A forked child gets its own owner id and worker slot, and inherits the parent's pins. */
void nocopy_after_fork(nocopy_ctx *c)
{
	ar_proc_after_fork();
	if (nocopy_claim(c)) {
		ar_pins_publish(&c->pins);
	}
}

void nocopy_request_end(nocopy_ctx *c)
{
	if (c->atom.anest) { /* a write bailed out between nocopy_atom_begin and _end */
		c->atom.anest = 1;
		ar_atom_end(&c->atom, c->pins.slot);
	}
	c->pin_refs = 0;
	ar_pins_drop_all(&c->pins);
}

/* ---- iteration ---- */

void nocopy_each(nocopy_ctx *c, void (*fn)(const nocopy_entry *e, void *arg), void *arg)
{
	time_t now = nocopy_now();
	for (uint32_t si = 0; si <= ar_shard_mask; si++) {
		const ar_index *ix = &ar_ix[si];
		for (uint32_t i = 0; i <= ix->mask; i++) {
			ar_slot s = ar_index_load(ix, i);
			if (!s) {
				continue;
			}
			nocopy_entry *e = ar_at8(ar_slot_off8(s));
			if (ar_pin(&c->pins, e) && ar_index_load(ix, i) != s) {
				continue; /* unlinked while pinning: the memory may be reused */
			}
			if (!e->expires || e->expires >= now) {
				fn(e, arg);
			}
		}
	}
}

/* ---- stats ---- */

void nocopy_stats(nocopy_stats_t *st)
{
	memset(st, 0, sizeof *st);
	time_t now = nocopy_now();
	ar_shards_stats sh;
	ar_shards_stat(&sh);
	st->used = sh.used;
	st->entries = sh.entries;
	st->evacuated = sh.evacuated;
	st->evicted = sh.evicted;
	st->expired = sh.expired;
	st->ghost_hits = sh.ghost_hits;
	st->dead_segments = sh.dead_segments;
	uint32_t nw = ar_worker_high();
	for (uint32_t i = 0; i < nw; i++) {
		st->hits += __atomic_load_n(&ar_worker_at(i)->hits, __ATOMIC_RELAXED);
		st->misses += __atomic_load_n(&ar_worker_at(i)->misses, __ATOMIC_RELAXED);
	}
	st->pinned_segments = ar_pool_pinned_count();
	st->segments = ar_nsegs;
	st->free_segments = ar_pool_free_count();
	st->segment_size = ar_seg_size;
	st->start_time = ar_hdr->start_time;
	st->shards = ar_hdr->nshards;
	st->slots = (uint64_t) ar_hdr->nshards * ar_hdr->slots_per;
	st->expunges = nocopy_expunges();
	st->lock_steals = __atomic_load_n(&ar_proc->lock_steals, __ATOMIC_RELAXED);
	st->leases = ar_leases_held(now);
	ar_atoms_stats a;
	ar_atoms_stat(&a);
	st->atoms = a.atoms;
	st->atoms_used = a.used;
	st->atoms_size = a.size;
	st->atom_rotations = a.rotations;
}

uint64_t nocopy_expunges(void) { return __atomic_load_n(&ar_hdr->expunges, __ATOMIC_RELAXED); }
