/* nocopy: the store API (nocopy_php.h) over the arena: copy mode's private copies, writes, reads, single-flight. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include <math.h>
#include <time.h>
#include <unistd.h>

size_t nocopy_local_size = 8 * 1024 * 1024;
bool nocopy_detour;

bool nocopy_available(void)
{
	return nocopy_a != NULL;
}

bool nocopy_tracked(void)
{
	if (NOCOPY_G(ctx).slot < 0 && nocopy_a) { /* no arena (unavailable): the store API fails softly instead */
		zend_throw_error(NULL, "nocopy: no free worker slot");
		return false;
	}
	return true;
}

/* ---- copy mode: private copies kept for the request ---- */

/* The slot for e's copy, or NULL: nothing worth keeping, or not in a request. */
static zend_always_inline nocopy_local *local_way(const nocopy_entry *e);

static zend_always_inline nocopy_local *local_slot(const nocopy_entry *e, const zval *v)
{
	/* short strings copy faster than a cold slot line loads (100k distinct 100 B strings: 37 -> 45 ns kept) */
	if ((uint32_t) (Z_TYPE_P(v) - IS_STRING) > 1 || !NOCOPY_G(local_ok) || e->size > nocopy_local_size
	 || (Z_TYPE_P(v) == IS_STRING ? e->size < 512 : !zend_hash_num_elements(Z_ARR_P(v)))) {
		return NULL;
	}
	return local_way(e);
}

/* Graphs keep a template (IS_PTR): objects are rebuilt from it on every fetch. */
static void local_val_free(zval *v)
{
	if (Z_TYPE_P(v) == IS_PTR) {
		nocopy_graph_tpl_free(Z_PTR_P(v));
	} else {
		zval_ptr_dtor(v);
	}
}

static zend_always_inline nocopy_local *local_way(const nocopy_entry *e)
{
	if (UNEXPECTED(!NOCOPY_G(local))) {
		NOCOPY_G(local) = ecalloc(NOCOPY_LOCAL, sizeof(nocopy_local));
	}
	nocopy_local *set = &NOCOPY_G(local)[((((uintptr_t) e >> 3) * 0x9E3779B97F4A7C15ULL) >> 54) * NOCOPY_LOCAL_WAYS], *victim = set;
	int best = 4;
	for (int i = 0; i < NOCOPY_LOCAL_WAYS; i++) {
		nocopy_local *l = &set[i];
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

static void local_flush(void)
{
	for (int i = 0; i < NOCOPY_LOCAL; i++) {
		if (NOCOPY_G(local)[i].e) {
			local_val_free(&NOCOPY_G(local)[i].v);
			NOCOPY_G(local)[i].e = NULL;
		}
	}
	NOCOPY_G(local_bytes) = 0;
}

static void local_drop(nocopy_local *l)
{
	NOCOPY_G(local_bytes) -= l->size;
	local_val_free(&l->v);
	l->e = NULL;
}

/* Over budget: SIEVE evicts copies not used since the hand last passed (2 rounds at most: the first clears every bit). */
static void local_put(nocopy_local *l, const nocopy_entry *e, const zval *copy)
{
	if (l->e) {
		local_drop(l);
	}
	for (uint32_t n = 0; NOCOPY_G(local_bytes) + e->size > nocopy_local_size && n < 2 * NOCOPY_LOCAL; n++) {
		nocopy_local *h = &NOCOPY_G(local)[NOCOPY_G(local_hand)++ % NOCOPY_LOCAL];
		if (h->visited) {
			h->visited = false;
		} else if (h->e) {
			local_drop(h);
		}
	}
	l->e = e;
	l->seq = e->seq;
	l->size = e->size;
	l->visited = false;
	ZVAL_COPY(&l->v, copy);
	NOCOPY_G(local_bytes) += e->size;
}

/* Copy mode: from the kept template if this version has one; a second sighting keeps one. */
static bool graph_copy_fetch(const nocopy_entry *e, const zval *v, zval *rv)
{
	nocopy_local *l = NOCOPY_G(local_ok) && e->size <= nocopy_local_size ? local_way(e) : NULL;
	if (l && l->e == e && l->seq == e->seq) {
		l->visited = true;
		return nocopy_graph_tpl_out(rv, Z_PTR(l->v));
	}
	if (l && l->seen == e) {
		void *t = nocopy_graph_tpl(e);
		if (t) {
			zval z;
			ZVAL_PTR(&z, t);
			local_put(l, e, &z);
			return nocopy_graph_tpl_out(rv, t);
		}
	} else if (l) {
		l->seen = e;
	}
	return nocopy_graph_out(rv, v, true);
}

/* ---- writes ---- */

nocopy_status nocopy_publish(nocopy_put *p, bool add_only)
{
	switch (nocopy_put_commit(p, add_only)) {
		case PUT_OK: return NOCOPY_OK;
		case PUT_EXISTS: return NOCOPY_EXISTS;
		default: return NOCOPY_NO_ROOM;
	}
}

nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_a ? nocopy_vk_store(key, val, o, 0) : nocopy_build(NULL, key, val, o);
	}
	nocopy_put p;
	nocopy_status s = nocopy_build(&p, key, val, o);
	return s == NOCOPY_OK ? nocopy_publish(&p, o->add_only) : s;
}

static zend_always_inline bool drop_local(const char *k, size_t len)
{
	zend_ulong h = zend_hash_func(k, len); /* = zend_string_hash_val() */
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	bool found = nocopy_remove(sh, h, k, (uint32_t) len);
	nocopy_unlock(sh);
	return found;
}

bool nocopy_drop_local(const char *k, size_t len)
{
	return nocopy_a && drop_local(k, len);
}

bool nocopy_drop(zend_string *key)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_a && nocopy_vk_drop(key);
	}
	return drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
}

zend_long nocopy_drop_multi(HashTable *keys)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_a ? nocopy_vk_drop_multi(keys) : 0;
	}
	zend_long n = 0;
	zval *k;
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		n += Z_TYPE_P(k) == IS_STRING && drop_local(Z_STRVAL_P(k), Z_STRLEN_P(k));
	} ZEND_HASH_FOREACH_END();
	return n;
}

void nocopy_clear_all(void)
{
	if (UNEXPECTED(!nocopy_a)) {
		return;
	}
	if (nocopy_vk_on) {
		nocopy_vk_clear();
	}
	nocopy_clear();
}

bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out)
{
	if (UNEXPECTED(nocopy_detour)) {
		*out = 0;
		return nocopy_a && nocopy_vk_add_long(key, delta, ttl, out);
	}
	zend_ulong h = zend_string_hash_val(key);
	nocopy_shard *sh = nocopy_shard_of(h);
	for (;;) {
		nocopy_lock(sh);
		nocopy_entry *e = nocopy_lookup(sh, h, ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
		if (e && !nocopy_expired(e)) {
			zval *v = entry_val(e);
			bool is_long = Z_TYPE_P(v) == IS_LONG && !(e->flags & ENTRY_SERIALIZED);
			if (is_long) {
				*out = __atomic_add_fetch(&Z_LVAL_P(v), delta, __ATOMIC_RELAXED); /* in place: lock-free readers copy this word */
			}
			nocopy_unlock(sh);
			return is_long;
		}
		nocopy_unlock(sh);
		zval init;
		ZVAL_LONG(&init, delta);
		const nocopy_opts create = {.ttl = ttl, .add_only = true};
		nocopy_status s = nocopy_store(key, &init, &create);
		if (s != NOCOPY_EXISTS) {
			*out = delta;
			return s == NOCOPY_OK;
		}
		/* someone created it first: go around and update theirs */
	}
}

bool nocopy_cas_long(zend_string *key, zend_long old, zend_long nw)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_a && nocopy_vk_cas_long(key, old, nw);
	}
	zend_ulong h = zend_string_hash_val(key);
	nocopy_shard *sh = nocopy_shard_of(h);
	nocopy_lock(sh);
	nocopy_entry *e = nocopy_lookup(sh, h, ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	zval *v = e ? entry_val(e) : NULL;
	bool ok = e && !nocopy_expired(e) && !(e->flags & ENTRY_SERIALIZED) && Z_TYPE_P(v) == IS_LONG && Z_LVAL_P(v) == old;
	if (ok) {
		__atomic_store_n(&Z_LVAL_P(v), nw, __ATOMIC_RELAXED);
	}
	nocopy_unlock(sh);
	return ok;
}

/* ---- reads ---- */

/* Fetch into rv: zero-copy in a request-scoped request, else a private copy. exp/delta: the entry's expiry and compute time (ms). */
static zend_always_inline bool local_fetch_ex(zend_string *key, zval *rv, time_t *exp, uint32_t *delta, bool stale)
{
	nocopy_ctx *c = &NOCOPY_G(ctx);
	bool copy = !NOCOPY_G(zero_copy);
	nocopy_borrow_begin(c);
	nocopy_entry *e = nocopy_find_ex(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key), stale);
	bool hit = e != NULL;
	ZVAL_NULL(rv);
	if (e) {
		zval *v = entry_val(e);
		if (exp) {
			*exp = e->expires;
			*delta = e->user;
		}
		if (UNEXPECTED(e->flags & (ENTRY_SERIALIZED | ENTRY_GRAPH))) {
			if (e->flags & ENTRY_SERIALIZED) {
				hit = nocopy_unserialize(rv, Z_STRVAL_P(v), Z_STRLEN_P(v), e->flags);
			} else if (copy) {
				hit = graph_copy_fetch(e, v, rv);
			} else {
				hit = nocopy_graph_out(rv, v, false);
				nocopy_keep(c, e);
			}
		} else if (Z_TYPE_P(v) < IS_STRING) {
			ZVAL_COPY_VALUE(rv, v); /* a scalar: nothing to copy out or pin */
		} else if (copy) {
			nocopy_local *l = local_slot(e, v);
			if (l && l->e == e && l->seq == e->seq) {
				ZVAL_COPY(rv, &l->v); /* same version as last time: share it, COW keeps it intact */
				l->visited = true;
			} else {
				nocopy_keys *keys = &NOCOPY_G(keys);
				if (Z_TYPE_P(v) == IS_ARRAY) {
					keys_reset(keys);
				}
				nocopy_copy_out(rv, v);
				if (l && l->seen == e) {
					local_put(l, e, rv);
				} else if (l) {
					l->seen = e;
				}
			}
		} else {
			ZVAL_COPY_VALUE(rv, v);
			nocopy_keep(c, e); /* a pointer into the arena escaped: stay pinned until the request ends */
		}
	}
	uint64_t *ctr = hit ? &nocopy_a->workers[c->slot].hits : &nocopy_a->workers[c->slot].misses;
	__atomic_store_n(ctr, *ctr + 1, __ATOMIC_RELAXED); /* single writer per slot: no RMW needed */
	nocopy_borrow_end(c);
	return hit;
}

static zend_always_inline bool local_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	return local_fetch_ex(key, rv, exp, delta, false);
}

bool nocopy_fetch_local(zend_string *key, zval *rv, time_t *exp, uint32_t *delta) { return local_fetch(key, rv, exp, delta); }

static zend_always_inline bool fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (UNEXPECTED(nocopy_detour)) {
		if (!nocopy_a) {
			ZVAL_NULL(rv);
			return false;
		}
		return nocopy_vk_fetch(key, rv, exp, delta);
	}
	return local_fetch(key, rv, exp, delta);
}

bool nocopy_fetch(zend_string *key, zval *rv) { return fetch(key, rv, NULL, NULL); }

/* noinline: under LTO, inlined into nocopy_get_multiple, copy mode got 14% slower */
__attribute__((noinline)) void nocopy_fetch_multi(HashTable *keys, HashTable *out)
{
	if (UNEXPECTED(nocopy_detour)) {
		if (nocopy_a) {
			nocopy_vk_fetch_multi(keys, out);
		}
		return;
	}
	zval *k, t;
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		if (Z_TYPE_P(k) == IS_STRING && local_fetch(Z_STR_P(k), &t, NULL, NULL)) {
			zend_symtable_update(out, Z_STR_P(k), &t);
		}
	} ZEND_HASH_FOREACH_END();
}

void nocopy_mark_failed(HashTable *failed, zend_string *key)
{
	if (failed && !EG(exception)) {
		zval m;
		ZVAL_LONG(&m, -1);
		zend_symtable_update(failed, key, &m);
	}
}

bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed)
{
	if (nocopy_vk_on && !o->add_only && nocopy_a) {
		return nocopy_vk_store_multi(vals, o, failed); /* ponytail: add stays one round trip per key (NX decides first) */
	}
	zend_ulong h;
	zend_string *k;
	zval *v;
	bool all = true;
	ZEND_HASH_FOREACH_KEY_VAL(vals, h, k, v) {
		zend_string *key = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		if (nocopy_store(key, v, o) != NOCOPY_OK) {
			all = false;
			nocopy_mark_failed(failed, key);
		}
		zend_string_release(key);
		if (EG(exception)) {
			break;
		}
	} ZEND_HASH_FOREACH_END();
	return all;
}

/* ---- single-flight: a lease per key hash (in the arena's shard) holds who computes it ---- */

#define LEASE_TTL 30 /* s: a live holder stuck longer than this stops blocking the others */

static double now_s(clockid_t clk)
{
	struct timespec ts;
	clock_gettime(clk, &ts);
	return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static int lease_take(zend_ulong h)
{
	if (NOCOPY_G(nleases) == (int) (sizeof NOCOPY_G(leases) / sizeof NOCOPY_G(leases)[0])) {
		return NOCOPY_LEASE_FULL; /* nested too deep: compute without one */
	}
	int r = nocopy_lease_take(h, NOCOPY_G(ctx).slot, nocopy_now(), LEASE_TTL);
	if (r == NOCOPY_LEASE_TAKEN) {
		NOCOPY_G(leases)[NOCOPY_G(nleases)++] = h;
	}
	return r;
}

static void lease_drop(void)
{
	nocopy_lease_drop(NOCOPY_G(leases)[--NOCOPY_G(nleases)], NOCOPY_G(ctx).slot);
}

/* Runs the callback (resolved only now: hits never call it), measures it, stores the result with its compute time. */
static void compute(zend_string *key, zval *fn, const nocopy_opts *o, zval *rv)
{
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	if (zend_fcall_info_init(fn, 0, &fci, &fcc, NULL, NULL) != SUCCESS) {
		return; /* checked by the caller */
	}
	zval args[2], t;
	ZVAL_STR(&args[0], key);
	ZVAL_LONG(&t, o->ttl);
	ZVAL_NEW_REF(&args[1], &t);
	fci.param_count = fcc.function_handler->type == ZEND_USER_FUNCTION ? 2 : 1; /* internal ones reject extra args */
	fci.params = args;
	fci.retval = rv;
	double t0 = now_s(CLOCK_MONOTONIC);
	zend_result r = zend_call_function(&fci, &fcc);
	zend_release_fcall_info_cache(&fcc);
	zend_long ttl = zval_get_long(Z_REFVAL(args[1]));
	zval_ptr_dtor(&args[1]);
	if (r == FAILURE || EG(exception) || ttl < 0) {
		return; /* ttl < 0: the callback asks not to store */
	}
	const nocopy_opts oo = {.ttl = ttl, .add_only = o->add_only};
	o = &oo;
	double ms = (now_s(CLOCK_MONOTONIC) - t0) * 1000;
	uint32_t user = ms >= UINT32_MAX ? UINT32_MAX : (uint32_t) ms;
	if (UNEXPECTED(!nocopy_a)) {
		nocopy_build(NULL, key, rv, o);
		return;
	}
	if (nocopy_vk_on) {
		nocopy_vk_store(key, rv, o, user);
		return;
	}
	nocopy_put p;
	if (nocopy_build(&p, key, rv, o) == NOCOPY_OK) {
		p.e->user = user;
		nocopy_publish(&p, o->add_only);
	}
}

void nocopy_remember(zend_string *key, zval *fn, const nocopy_opts *o, double beta, zval *rv)
{
	time_t exp;
	uint32_t delta;
	if (UNEXPECTED(nocopy_detour) && !nocopy_a) {
		compute(key, fn, o, rv); /* no leases without an arena */
		return;
	}
	if (fetch(key, rv, &exp, &delta)) {
		if (!(beta > 0 && exp && delta)) {
			return;
		}
		double r = ((nocopy_rand(&NOCOPY_G(ctx)) >> 8) + 1) / 16777216.0; /* (0, 1] */
		if (now_s(CLOCK_REALTIME) - delta / 1000.0 * beta * log(r) < (double) exp + 1) { /* expires at the end of second exp */
			return; /* XFetch: not yet */
		}
		if (lease_take(zend_string_hash_val(key)) != NOCOPY_LEASE_TAKEN) {
			return; /* someone else refreshes: keep serving this value */
		}
		zval_ptr_dtor(rv);
		ZVAL_NULL(rv);
		compute(key, fn, o, rv);
		lease_drop();
		return;
	}
	zend_ulong h = zend_string_hash_val(key);
	double deadline = now_s(CLOCK_MONOTONIC) + LEASE_TTL;
	unsigned wait = 50;
	bool mine = false;
	for (;;) {
		int r = lease_take(h);
		if (r != NOCOPY_LEASE_HELD) {
			mine = r == NOCOPY_LEASE_TAKEN; /* MINE: our own callback asks for its key; FULL: compute without */
			break;
		}
		if (fetch(key, rv, NULL, NULL)) {
			return; /* the holder stored it */
		}
		if (beta > 0 && !nocopy_detour && local_fetch_ex(key, rv, NULL, NULL, true)) {
			return; /* stale-while-revalidate: the expired value, while the holder computes the new one */
		}
		if (now_s(CLOCK_MONOTONIC) > deadline) {
			break;
		}
		usleep(wait);
		wait = wait < 5000 ? wait * 2 : wait;
	}
	if (mine && fetch(key, rv, NULL, NULL)) {
		lease_drop(); /* stored between our miss and the lease */
		return;
	}
	compute(key, fn, o, rv);
	if (mine) {
		lease_drop();
	}
}


void nocopy_store_rshutdown(void)
{
	NOCOPY_G(local_ok) = false; /* later fetches (session's RSHUTDOWN) copy without keeping */
	if (NOCOPY_G(local)) {
		local_flush();
		efree(NOCOPY_G(local));
		NOCOPY_G(local) = NULL;
	}
}

void nocopy_leases_drop(void)
{
	while (NOCOPY_G(nleases)) {
		lease_drop(); /* a callback bailed out (fatal, timeout) while holding it */
	}
}
