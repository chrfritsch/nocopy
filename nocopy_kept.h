/* Copy mode's reads and kept copies: private copies of arena values for the request, by entry version (address + seq). nocopy_kept.c. */

#ifndef NOCOPY_KEPT_H
#define NOCOPY_KEPT_H

#include "nocopy_php.h"

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

extern size_t nocopy_kept_size; /* nocopy.local_size */

void nocopy_kept_put(nocopy_kept *way, const nocopy_entry *e, const zval *copy); /* a copy, or a graph template (IS_PTR) */
void nocopy_kept_rinit(void);
void nocopy_kept_rshutdown(void); /* later fetches (session's RSHUTDOWN) copy without keeping */

/* The way for e's copy in its set: e's own, else the best victim. */
static zend_always_inline nocopy_kept *nocopy_kept_way(const nocopy_entry *e)
{
	if (UNEXPECTED(!NOCOPY_G(kept))) {
		NOCOPY_G(kept) = ecalloc(NOCOPY_KEPT, sizeof(nocopy_kept));
	}
	nocopy_kept *set = &NOCOPY_G(kept)[((((uintptr_t) e >> 3) * 0x9E3779B97F4A7C15ULL) >> 54) * NOCOPY_KEPT_WAYS], *victim = set;
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
static zend_always_inline nocopy_kept *nocopy_kept_get(const nocopy_entry *e, const zval *v, const zval **hit)
{
	/* short strings copy faster than a cold slot line loads (100k distinct 100 B strings: 37 -> 45 ns kept) */
	if ((uint32_t) (Z_TYPE_P(v) - IS_STRING) > 1 || !NOCOPY_G(kept_ok) || nocopy_entry_size(e) > nocopy_kept_size
	 || (Z_TYPE_P(v) == IS_STRING ? nocopy_entry_size(e) < 512 : !zend_hash_num_elements(Z_ARR_P(v)))) {
		*hit = NULL;
		return NULL;
	}
	return nocopy_kept_admit(nocopy_kept_way(e), e, hit);
}

/* A graph entry: the same for its template (design-decisions: one shared check made other fetches 2% slower). */
static zend_always_inline nocopy_kept *nocopy_kept_get_graph(const nocopy_entry *e, const zval **hit)
{
	return nocopy_kept_admit(NOCOPY_G(kept_ok) && nocopy_entry_size(e) <= nocopy_kept_size ? nocopy_kept_way(e) : NULL, e, hit);
}

/* A graph: from the kept template if this version has one; a second sighting keeps one (IS_PTR). */
static inline bool nocopy_kept_graph(const nocopy_entry *e, zval *rv)
{
	nocopy_env *env = &NOCOPY_G(env);
	const zval *k;
	nocopy_kept *w = nocopy_kept_get_graph(e, &k);
	if (k) {
		return nocopy_tpl_read(env, Z_PTR_P(k), rv);
	}
	nocopy_tpl *t = w ? nocopy_tpl_make(env, e) : NULL;
	if (t) {
		zval z;
		ZVAL_PTR(&z, t);
		nocopy_kept_put(w, e, &z);
		return nocopy_tpl_read(env, t, rv);
	}
	return nocopy_value_read(env, e, true, rv) & NOCOPY_HIT;
}

/* Copy mode's read: e's kept copy, else a private copy (kept on its second sighting). */
static zend_always_inline bool nocopy_kept_read(const nocopy_entry *e, zval *rv)
{
	const zval *v = nocopy_value_zval(e);
	if (UNEXPECTED(nocopy_value_fmt(e) != NOCOPY_FMT_PLAIN)) {
		return nocopy_value_fmt(e) == NOCOPY_FMT_GRAPH ? nocopy_kept_graph(e, rv) : nocopy_value_read(&NOCOPY_G(env), e, true, rv) & NOCOPY_HIT;
	}
	if (Z_TYPE_P(v) < IS_STRING) {
		ZVAL_COPY_VALUE(rv, v); /* a scalar: nothing to copy */
		return true;
	}
	const zval *k;
	nocopy_kept *w = nocopy_kept_get(e, v, &k);
	if (k) {
		ZVAL_COPY(rv, k);
		return true;
	}
	nocopy_value_copy(&NOCOPY_G(env), rv, v);
	if (w) {
		nocopy_kept_put(w, e, rv);
	}
	return true;
}

#endif
