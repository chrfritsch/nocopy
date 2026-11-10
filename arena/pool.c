/* nocopy arena: the segment pool and the pins that keep segments from reuse. */

#include "pool.h"

char *ar_seg_base;
unsigned ar_seg_shift;
size_t ar_seg_size;
uint32_t ar_nsegs, ar_seg_words;
uint64_t *ar_pins;
uint32_t ar_pin_stride;
uint32_t *ar_unpin_gen;
uint8_t *ar_seg_owner;
ar_segmeta *ar_segmeta_at;

static size_t up64(size_t n) { return (n + 63) & ~(size_t) 63; }

size_t ar_pool_plan(ar_pool_hdr *h, size_t at, size_t maxsegs)
{
	h->pin_stride = (uint32_t) (((maxsegs + 63) / 64 + 7) & ~(size_t) 7);
	h->owner_off = up64(at);
	h->meta_off = up64(h->owner_off + maxsegs);
	h->pins_off = up64(h->meta_off + maxsegs * sizeof(ar_segmeta));
	return up64(h->pins_off + (size_t) NOCOPY_MAX_WORKERS * h->pin_stride * sizeof(uint64_t));
}

void ar_pool_init(ar_pool_hdr *h, size_t segs_off, uint32_t nsegs, unsigned shift)
{
	h->segs_off = segs_off;
	h->nsegs = nsegs;
	h->shift = shift;
}

void ar_pool_adopt(ar_pool_hdr *h)
{
	ar_seg_shift = h->shift;
	ar_seg_size = (size_t) 1 << h->shift;
	ar_seg_base = ar_base + h->segs_off;
	ar_nsegs = h->nsegs;
	ar_seg_words = (h->nsegs + 63) / 64;
	ar_seg_owner = (uint8_t *) ar_base + h->owner_off;
	ar_segmeta_at = (ar_segmeta *) (ar_base + h->meta_off);
	ar_pins = (uint64_t *) (ar_base + h->pins_off);
	ar_pin_stride = h->pin_stride;
	ar_unpin_gen = &h->unpin_gen;
}

uint32_t ar_pool_free_count(void)
{
	uint32_t n = 0;
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		n += !__atomic_load_n(&ar_seg_owner[s], __ATOMIC_RELAXED);
	}
	return n;
}

static bool claim(uint8_t tenant, uint32_t s)
{
	uint8_t z = 0;
	return __atomic_compare_exchange_n(&ar_seg_owner[s], &z, tenant, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void unclaim(uint32_t s) { __atomic_store_n(&ar_seg_owner[s], 0, __ATOMIC_RELEASE); }

uint32_t ar_pool_take(uint8_t tenant, uint32_t k, uint32_t from, uint32_t keep_free)
{
	if (ar_pool_free_count() < k + keep_free) {
		return AR_NONE;
	}
	uint32_t ns = ar_nsegs;
	for (uint32_t i = 0, n = 0; i < ns; i++) {
		uint32_t s = (from + i) % ns;
		n = __atomic_load_n(&ar_seg_owner[s], __ATOMIC_RELAXED) ? 0 : s ? n + 1 : 1; /* runs do not wrap */
		if (n < k) {
			continue;
		}
		uint32_t got = s + 1 - k, j = got;
		while (j <= s && claim(tenant, j)) {
			AR_HOOK("pool.claimed");
			j++;
		}
		if (j <= s) { /* another tenant took j first */
			for (uint32_t r = got; r < j; r++) {
				unclaim(r);
			}
			i -= s - j; /* resume after j */
			n = 0;
			continue;
		}
		return got;
	}
	return AR_NONE;
}

void ar_pool_give(uint32_t s, uint32_t k)
{
#ifdef NOCOPY_POISON
	memset(ar_seg_start(s), 0x5A, (size_t) k << ar_seg_shift); /* make use-after-free loud */
#endif
	for (uint32_t j = s; j < s + k; j++) {
		ar_segmeta_at[j] = (ar_segmeta) {.gen = ar_segmeta_at[j].gen + 1};
		unclaim(j);
		AR_HOOK("pool.given");
	}
}

bool ar_pool_any_deps(uint8_t bits)
{
	for (uint32_t s = 0; s < ar_nsegs; s++) {
		if (ar_segmeta_at[s].deps & bits) {
			return true;
		}
	}
	return false;
}

void ar_pins_clear(int slot)
{
	for (uint32_t w = 0; w < ar_seg_words; w++) {
		__atomic_store_n(&ar_pins_of(slot)[w], 0, __ATOMIC_SEQ_CST);
	}
}

void ar_pins_publish(const ar_pins_t *r)
{
	for (uint32_t w = 0; w < ar_seg_words; w++) {
		__atomic_store_n(&ar_pins_of(r->slot)[w], r->mine[w], __ATOMIC_SEQ_CST);
	}
}

/* Dead set first, pins second: a reader pins, then re-checks its slot; a segment that died after we read the pins
 * could have been pinned (and re-checked) just before. */
void ar_pool_pinned(const uint64_t *dead, uint64_t *held, time_t now)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	memset(held, 0, NOCOPY_SEG_WORDS * sizeof(uint64_t));
	uint32_t nw = ar_worker_high(), nwords = ar_seg_words;
	for (uint32_t i = 0; i < nw; i++) {
		uint64_t b[NOCOPY_SEG_WORDS], blocks = 0;
		for (uint32_t w = 0; w < nwords; w++) {
			b[w] = __atomic_load_n(&ar_pins_of((int) i)[w], __ATOMIC_SEQ_CST);
			blocks |= b[w] & dead[w];
		}
		if (!blocks) {
			continue;
		}
		if (ar_worker_dead(i, now)) {
			AR_HOOK("pins.dead");
			continue; /* died holding pins: not cleared, the slot may have a new owner by now (it clears them when it claims) */
		}
		for (uint32_t w = 0; w < nwords; w++) {
			held[w] |= b[w];
		}
	}
}

void ar_pool_ask_unpin(void) { __atomic_add_fetch(ar_unpin_gen, 1, __ATOMIC_RELAXED); }

uint32_t ar_pool_pinned_count(void)
{
	uint64_t held[NOCOPY_SEG_WORDS] = {0};
	uint32_t nw = ar_worker_high(), n = 0;
	for (uint32_t i = 0; i < nw; i++) {
		for (uint32_t w = 0; w < ar_seg_words; w++) {
			held[w] |= __atomic_load_n(&ar_pins_of((int) i)[w], __ATOMIC_SEQ_CST);
		}
	}
	for (uint32_t w = 0; w < ar_seg_words; w++) {
		n += (uint32_t) __builtin_popcountll(held[w]);
	}
	return n;
}
