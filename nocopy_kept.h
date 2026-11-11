/* Copy mode's reads and kept copies: private copies of arena values for the request, by entry version (address + seq). nocopy_kept.c.
 * No module state: the caller passes its cache and the value layer's env. */

#ifndef NOCOPY_KEPT_H
#define NOCOPY_KEPT_H

#include "nocopy_value.h"

#define NOCOPY_KEPT 4096 /* 1024 sets of NOCOPY_KEPT_WAYS */
#define NOCOPY_KEPT_WAYS 4

struct nocopy_kept {
	const nocopy_entry *e;	/* kept copy's entry, NULL none */
	const nocopy_entry *seen; /* last entry fetched here: kept only when it comes back (one-off fetches never evict) */
	uint64_t seq;
	size_t size;
	zval v;
	bool visited; /* SIEVE: used since the hand last passed */
};
typedef struct nocopy_kept nocopy_kept;

/* A thread's kept copies: request memory. */
typedef struct {
	nocopy_kept *ways; /* NULL until the first copy-mode fetch */
	size_t bytes;
	uint32_t hand; /* SIEVE hand over ways */
	bool ok;       /* between RINIT and RSHUTDOWN */
} nocopy_kept_cache;

extern size_t nocopy_kept_size; /* nocopy.local_size */

/* A copy, or a graph template (IS_PTR, taken over), of `size` heap bytes: an entry pointing at shared arrays is far smaller than its copy. */
void nocopy_kept_put(nocopy_kept_cache *k, nocopy_kept *way, const nocopy_entry *e, const zval *copy, size_t size);
void nocopy_kept_rinit(nocopy_kept_cache *k);
void nocopy_kept_rshutdown(nocopy_kept_cache *k); /* later fetches (session's RSHUTDOWN) copy without keeping */

/* The way for e's copy in its set: e's own, else the best victim. */
static zend_always_inline nocopy_kept *nocopy_kept_way(nocopy_kept_cache *k, const nocopy_entry *e)
{
	if (UNEXPECTED(!k->ways)) {
		k->ways = ecalloc(NOCOPY_KEPT, sizeof(nocopy_kept));
	}
	nocopy_kept *set = &k->ways[((((uintptr_t) e >> 3) * 0x9E3779B97F4A7C15ULL) >> 54) * NOCOPY_KEPT_WAYS], *victim = set;
	int best = 4;
	for (int i = 0; i < NOCOPY_KEPT_WAYS; i++) {
		nocopy_kept *l = &set[i];
		if (l->e == e || l->seen == e) {
			return l;
		}
		/* victim: a way with no copy and no pending sighting, else no copy, else one not used since the SIEVE hand passed */
		int rank = !l->e ? (l->seen ? 1 : 0) : (l->visited ? 3 : 2);
		if (rank < best) {
			best = rank;
			victim = l;
		}
	}
	return victim;
}

/* *hit: l's copy if it is this version of e. Else TinyLFU's doorkeeper: l on e's second sighting (keep the copy made now), the first is only remembered. */
static zend_always_inline nocopy_kept *nocopy_kept_admit(nocopy_kept *l, const nocopy_entry *e, const zval **hit)
{
	*hit = NULL;
	if (!l) {
		return NULL;
	}
	if (l->e == e && l->seq == nocopy_entry_version(e)) {
		l->visited = true;
		*hit = &l->v; /* share it: COW keeps it intact */
		return NULL;
	}
	if (l->seen == e) {
		return l;
	}
	l->seen = e;
	return NULL;
}

/* A plain value v of e: *hit its kept copy, else the way to keep a copy made now (NULL: not kept). */
static zend_always_inline nocopy_kept *nocopy_kept_get(nocopy_kept_cache *k, const nocopy_entry *e, const zval *v, const zval **hit)
{
	/* short strings copy faster than a cold slot line loads (100k distinct 100 B strings: 37 -> 45 ns kept) */
	if ((uint32_t) (Z_TYPE_P(v) - IS_STRING) > 1 || !k->ok || nocopy_entry_size(e) > nocopy_kept_size
	 || (Z_TYPE_P(v) == IS_STRING ? nocopy_entry_size(e) < 512 : !zend_hash_num_elements(Z_ARR_P(v)))) {
		*hit = NULL;
		return NULL;
	}
	return nocopy_kept_admit(nocopy_kept_way(k, e), e, hit);
}

/* A graph entry: the same for its template (design-decisions: one shared check made other fetches 2% slower). */
static zend_always_inline nocopy_kept *nocopy_kept_get_graph(nocopy_kept_cache *k, const nocopy_entry *e, const zval **hit)
{
	return nocopy_kept_admit(k->ok && nocopy_entry_size(e) <= nocopy_kept_size ? nocopy_kept_way(k, e) : NULL, e, hit);
}

/* A graph: from the kept template if this version has one; a second sighting keeps one (IS_PTR). */
static inline bool nocopy_kept_graph(nocopy_kept_cache *k, nocopy_env *env, const nocopy_entry *e, zval *rv)
{
	const zval *kz;
	nocopy_kept *w = nocopy_kept_get_graph(k, e, &kz);
	if (kz) {
		return nocopy_tpl_read(env, Z_PTR_P(kz), rv);
	}
	size_t m = w ? zend_memory_usage(0) : 0;
	nocopy_tpl *t = w ? nocopy_tpl_make(env, e) : NULL;
	if (t) {
		zval z;
		ZVAL_PTR(&z, t);
		nocopy_kept_put(k, w, e, &z, zend_memory_usage(0) - m);
		return w->e == e ? nocopy_tpl_read(env, t, rv) : nocopy_value_read(env, e, true, rv) & NOCOPY_HIT;
	}
	return nocopy_value_read(env, e, true, rv) & NOCOPY_HIT;
}

/* Copy mode's read: e's kept copy, else a private copy (kept on its second sighting). */
static zend_always_inline bool nocopy_kept_read(nocopy_kept_cache *k, nocopy_env *env, const nocopy_entry *e, zval *rv)
{
	const zval *v = nocopy_value_zval(e);
	if (UNEXPECTED(nocopy_value_fmt(e) != NOCOPY_FMT_PLAIN)) {
		return nocopy_value_fmt(e) == NOCOPY_FMT_GRAPH ? nocopy_kept_graph(k, env, e, rv) : nocopy_value_read(env, e, true, rv) & NOCOPY_HIT;
	}
	if (Z_TYPE_P(v) < IS_STRING) {
		ZVAL_COPY_VALUE(rv, v); /* a scalar: nothing to copy */
		return true;
	}
	const zval *kz;
	nocopy_kept *w = nocopy_kept_get(k, e, v, &kz);
	if (kz) {
		ZVAL_COPY(rv, kz);
		return true;
	}
	size_t m = w ? zend_memory_usage(0) : 0;
	nocopy_value_copy(env, rv, v);
	if (w) {
		nocopy_kept_put(k, w, e, rv, zend_memory_usage(0) - m);
	}
	return true;
}

#endif
