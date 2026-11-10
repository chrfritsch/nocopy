/* nocopy arena: a shard's slot table, writer side. */

#include "index.h"

void ar_index_init(ar_index_hdr *h, uint32_t slots) { h->cap = slots / 4 * 3; }

void ar_index_adopt(ar_index *ix, ar_index_hdr *h, ar_slot *t, uint32_t slots)
{
	ix->t = t;
	ix->hdr = h;
	ix->mask = slots - 1;
}

/* Backward shift: entries of the run behind the hole move into it. Readers that miss while a key moves back past them retry (mseq). */
void ar_index_remove(ar_index *ix, ar_slot *at)
{
	ar_slot *t = ix->t;
	uint32_t m = ix->mask, i = (uint32_t) (at - t);
	uint32_t *mseq = &ix->hdr->mseq;
	bool shifting = false;
	for (uint32_t j = (i + 1) & m;; j = (j + 1) & m) {
		ar_slot s = t[j];
		if (!s) {
			break;
		}
		uint32_t home = (uint32_t) (s >> 32) & m;
		if (((j - home) & m) >= ((j - i) & m)) { /* i lies between its home and j: move it back */
			if (!shifting) {
				shifting = true;
				__atomic_store_n(mseq, *mseq + 1, __ATOMIC_RELAXED);
				__atomic_thread_fence(__ATOMIC_RELEASE);
				AR_HOOK("shift.odd");
			}
			__atomic_store_n(&t[i], s, __ATOMIC_RELEASE);
			AR_HOOK("shift.moved");
			i = j;
		}
	}
	__atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
	if (shifting) {
		__atomic_store_n(mseq, *mseq + 1, __ATOMIC_RELEASE);
	}
	ix->hdr->nlive--;
}

ar_slot *ar_index_find(const ar_index *ix, ar_slot v)
{
	for (uint32_t i = (uint32_t) (v >> 32) & ix->mask;; i = (i + 1) & ix->mask) {
		if (!ix->t[i]) {
			return NULL;
		}
		if (ar_slot_off8(ix->t[i]) == ar_slot_off8(v)) {
			return &ix->t[i];
		}
	}
}

void ar_index_clear(ar_index *ix, void (*gone)(ar_slot s, void *arg), void *arg)
{
	for (uint32_t i = 0; i <= ix->mask; i++) {
		ar_slot s = ix->t[i];
		__atomic_store_n(&ix->t[i], 0, __ATOMIC_RELEASE);
		if (s) {
			gone(s, arg);
		}
	}
	ix->hdr->nlive = 0;
}

void ar_index_repair(ar_index *ix)
{
	ar_slot *t = ix->t;
	if (ix->hdr->mseq & 1) {
		__atomic_store_n(&ix->hdr->mseq, ix->hdr->mseq + 1, __ATOMIC_RELEASE); /* else readers retry every miss forever */
	}
	for (uint32_t i = 0; i <= ix->mask; i++) {
		while (t[i] && ar_index_find(ix, t[i]) != &t[i]) {
			ar_index_remove(ix, &t[i]); /* a key the shift copied back but did not yet clear */
		}
	}
	uint32_t n = 0;
	for (uint32_t i = 0; i <= ix->mask; i++) {
		n += t[i] != 0;
	}
	ix->hdr->nlive = n;
}
