/* nocopy: copy mode's kept copies (nocopy_kept.h): eviction (SIEVE) under a byte budget, freed at request end. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_kept.h"

size_t nocopy_kept_size = 8 * 1024 * 1024;

/* Graphs keep a template (IS_PTR): objects are rebuilt from it on every fetch. */
static void val_free(zval *v)
{
	if (Z_TYPE_P(v) == IS_PTR) {
		nocopy_graph_tpl_free(Z_PTR_P(v));
	} else {
		zval_ptr_dtor(v);
	}
}

static void drop(nocopy_kept *l)
{
	NOCOPY_G(kept_bytes) -= l->size;
	val_free(&l->v);
	l->e = NULL;
}

/* Over budget: SIEVE evicts copies not used since the hand last passed (2 rounds at most: the first clears every bit). */
void nocopy_kept_put(nocopy_kept *l, const nocopy_entry *e, const zval *copy)
{
	if (l->e) {
		drop(l);
	}
	for (uint32_t n = 0; NOCOPY_G(kept_bytes) + e->size > nocopy_kept_size && n < 2 * NOCOPY_KEPT; n++) {
		nocopy_kept *h = &NOCOPY_G(kept)[NOCOPY_G(kept_hand)++ % NOCOPY_KEPT];
		if (h->visited) {
			h->visited = false;
		} else if (h->e) {
			drop(h);
		}
	}
	l->e = e;
	l->seq = e->seq;
	l->size = e->size;
	l->visited = false;
	ZVAL_COPY(&l->v, copy);
	NOCOPY_G(kept_bytes) += e->size;
}

void nocopy_kept_rinit(void) { NOCOPY_G(kept_ok) = nocopy_kept_size > 0; }

void nocopy_kept_rshutdown(void)
{
	NOCOPY_G(kept_ok) = false;
	if (!NOCOPY_G(kept)) {
		return;
	}
	for (int i = 0; i < NOCOPY_KEPT; i++) {
		if (NOCOPY_G(kept)[i].e) {
			val_free(&NOCOPY_G(kept)[i].v);
		}
	}
	efree(NOCOPY_G(kept));
	NOCOPY_G(kept) = NULL;
	NOCOPY_G(kept_bytes) = 0;
}
