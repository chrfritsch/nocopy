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
		nocopy_tpl_free(Z_PTR_P(v));
	} else {
		zval_ptr_dtor(v);
	}
}

static void drop(nocopy_kept_cache *k, nocopy_kept *l)
{
	k->bytes -= l->size;
	val_free(&l->v);
	l->e = NULL;
}

/* Over budget: SIEVE evicts copies not used since the hand last passed (2 rounds at most: the first clears every bit). */
void nocopy_kept_put(nocopy_kept_cache *k, nocopy_kept *l, const nocopy_entry *e, const zval *copy, size_t size)
{
	if (l->e) {
		drop(k, l);
	}
	if (size < nocopy_entry_size(e)) {
		size = nocopy_entry_size(e); /* a heap without statistics */
	}
	if (size > nocopy_kept_size) {
		if (Z_TYPE_P(copy) == IS_PTR) {
			nocopy_tpl_free(Z_PTR_P(copy));
		}
		return;
	}
	for (uint32_t n = 0; k->bytes + size > nocopy_kept_size && n < 2 * NOCOPY_KEPT; n++) {
		nocopy_kept *h = &k->ways[k->hand++ % NOCOPY_KEPT];
		if (h->visited) {
			h->visited = false;
		} else if (h->e) {
			drop(k, h);
		}
	}
	l->e = e;
	l->seq = nocopy_entry_version(e);
	l->size = size;
	l->visited = false;
	ZVAL_COPY(&l->v, copy);
	k->bytes += size;
}

void nocopy_kept_rinit(nocopy_kept_cache *k) { k->ok = nocopy_kept_size > 0; }

void nocopy_kept_rshutdown(nocopy_kept_cache *k)
{
	k->ok = false;
	if (!k->ways) {
		return;
	}
	for (int i = 0; i < NOCOPY_KEPT; i++) {
		if (k->ways[i].e) {
			val_free(&k->ways[i].v);
		}
	}
	efree(k->ways);
	k->ways = NULL;
	k->bytes = 0;
}
