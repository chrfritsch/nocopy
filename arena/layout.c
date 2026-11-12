/* nocopy arena: planning the region, setting up a fresh one, adopting an existing one. */

#include "layout.h"

char *ar_base;
size_t ar_size;
ar_arena *ar_hdr;

static size_t up64(size_t n) { return (n + 63) & ~(size_t) 63; }

nocopy_err ar_layout_plan(ar_layout *l, size_t size, uint64_t hint)
{
	memset(l, 0, sizeof *l);
	unsigned shift = AR_SEG_MIN_SHIFT;
	while ((size >> shift) > 256 && shift < AR_SEG_MAX_SHIFT) {
		shift++;
	}
	while ((size >> shift) > NOCOPY_MAX_SEGS) {
		shift++;
	}
	uint32_t nshards = 1;
	while (nshards < NOCOPY_MAX_SHARDS && size / (nshards * 2) >= ((size_t) 8 << 20) && (size >> shift) / (nshards * 2) >= 16) {
		nshards *= 2;
	}
	uint64_t per = 1024;
	while (per <= ((uint64_t) 1 << 31) && (per * nshards / 2 < hint || per * nshards < size / 256)) { /* bounded: a huge hint fails below */
		per <<= 1;
	}
	size_t maxsegs = size >> shift; /* an upper bound: the header takes some */
	l->tables_off = up64(sizeof(ar_arena));
	l->ghosts_off = l->tables_off + (size_t) nshards * per * sizeof(ar_slot);
	l->segrec_off = up64(l->ghosts_off + (size_t) nshards * per * sizeof(uint16_t));
	size_t at = ar_pool_plan(&l->pool, l->segrec_off + maxsegs * sizeof(ar_seg), maxsegs);
	l->header = ar_atoms_plan(&l->atoms, at, size);
	size_t need = l->header + ((size_t) 4 << shift);
	if (per > ((uint64_t) 1 << 31) || size < need) {
		return (nocopy_err) {.code = NOCOPY_ERR_TOO_SMALL, .msg = "shm_size is too small or entries_hint too large", .need = need};
	}
	if (size > ((size_t) 32 << 30)) {
		return (nocopy_err) {.code = NOCOPY_ERR_FAILED, .msg = "larger than 32G"};
	}
	l->size = size;
	l->shift = shift;
	l->nshards = nshards;
	l->slots_per = (uint32_t) per;
	return (nocopy_err) {.code = NOCOPY_ERR_NONE};
}

void ar_layout_init(ar_arena *a, const ar_layout *l)
{
	uint32_t nsegs = (uint32_t) ((l->size - l->header) >> l->shift);
	a->header_size = l->header;
	a->nshards = l->nshards;
	a->slots_per = l->slots_per;
	a->tables_off = l->tables_off;
	a->ghosts_off = l->ghosts_off;
	a->segrec_off = l->segrec_off;
	a->pool = l->pool;
	ar_pool_init(&a->pool, l->header, nsegs, l->shift);
	a->atoms = l->atoms;
	ar_atoms_init(&a->atoms);
	ar_shards_init(a->shards, l->nshards, l->slots_per, nsegs);
	a->start_time = time(NULL);
}

bool ar_layout_adopt(ar_arena *a, nocopy_rebase_fn rebase)
{
	uint32_t n = a->nshards;
	if (!n || n > NOCOPY_MAX_SHARDS || (n & (n - 1))) {
		return false;
	}
	ar_hdr = a;
	ar_base = (char *) a;
	ar_size = a->region.size;
	ar_proc_adopt(&a->procs);
	ar_pool_adopt(&a->pool);
	ar_log_adopt((ar_seg *) (ar_base + a->segrec_off), n);
	ar_shards_adopt(a->shards, n, (ar_slot *) (ar_base + a->tables_off), (uint16_t *) (ar_base + a->ghosts_off), a->slots_per, &a->expunges, rebase);
	ar_leases_adopt(a->leases, n);
	ar_atoms_adopt(&a->atoms, ar_shards_still_depend);
	return true;
}
