/* nocopy: the store API (nocopy_php.h) over the arena: writes, reads (copy mode's through nocopy_kept.h), single-flight. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include <math.h>
#include <time.h>
#include <unistd.h>

const nocopy_detour_ops *nocopy_detour;

bool nocopy_tracked(void)
{
	if (nocopy_ctx_slot(&NOCOPY_G(ctx)) < 0 && nocopy_attached()) { /* no arena (unavailable): the store API fails softly instead */
		zend_throw_error(NULL, "nocopy: no free worker slot");
		return false;
	}
	return true;
}

/* ---- writes ---- */

nocopy_status nocopy_put_local(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user)
{
	nocopy_put_req q = {.h = zend_string_hash_val(key), .key = ZSTR_VAL(key), .klen = (uint32_t) ZSTR_LEN(key),
		.ttl = (time_t) o->ttl, .expires = o->expires, .user = user};
	return nocopy_value_put(&NOCOPY_G(env), &NOCOPY_G(ctx), &q, val, o->add_only);
}

/* user: the compute time (ms). */
static zend_always_inline nocopy_status store_as(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_detour->store(key, val, o, user);
	}
	return nocopy_put_local(key, val, o, user);
}

nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o) { return store_as(key, val, o, 0); }

static zend_always_inline bool drop_local(const char *k, size_t len, zend_ulong h) { return nocopy_delete(h, k, (uint32_t) len); }

bool nocopy_drop_local(const char *k, size_t len)
{
	return drop_local(k, len, zend_hash_func(k, len)); /* = zend_string_hash_val() */
}

bool nocopy_drop(zend_string *key)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_detour->drop(key);
	}
	return drop_local(ZSTR_VAL(key), ZSTR_LEN(key), zend_string_hash_val(key));
}

zend_long nocopy_drop_multi(HashTable *keys)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_detour->drop_multi(keys);
	}
	zend_long n = 0;
	zval *k;
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		n += Z_TYPE_P(k) == IS_STRING && drop_local(Z_STRVAL_P(k), Z_STRLEN_P(k), zend_string_hash_val(Z_STR_P(k)));
	} ZEND_HASH_FOREACH_END();
	return n;
}

void nocopy_clear_all(void)
{
	if (UNEXPECTED(nocopy_detour)) {
		nocopy_detour->clear();
		return;
	}
	nocopy_clear();
}

typedef struct { zend_long a, b; } long_op; /* add: delta, result; cas: old, new */

static int add_fn(nocopy_entry *e, void *arg)
{
	long_op *o = arg;
	zval *v = nocopy_value_long(e); /* changed in place under the shard lock: lock-free readers copy the word */
	if (!v) {
		return 0;
	}
	o->b = __atomic_add_fetch(&Z_LVAL_P(v), o->a, __ATOMIC_RELAXED);
	return 1;
}

static int cas_fn(nocopy_entry *e, void *arg)
{
	long_op *o = arg;
	zval *v = nocopy_value_long(e);
	if (!v || Z_LVAL_P(v) != o->a) {
		return 0;
	}
	__atomic_store_n(&Z_LVAL_P(v), o->b, __ATOMIC_RELAXED);
	return 1;
}

bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_detour->add_long(key, delta, ttl, out);
	}
	zend_ulong h = zend_string_hash_val(key);
	for (;;) {
		long_op op = {.a = delta};
		int r = nocopy_update(h, ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key), add_fn, &op);
		if (r != NOCOPY_GONE) {
			*out = op.b;
			return r;
		}
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
		return nocopy_detour->cas_long(key, old, nw);
	}
	long_op op = {.a = old, .b = nw};
	return nocopy_update(zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key), cas_fn, &op) == 1;
}

/* ---- reads ---- */

/* Fetch into rv: zero-copy in a request-scoped request, else a private copy. exp/delta: the entry's expiry and compute time (ms). */
static zend_always_inline bool local_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta, bool stale)
{
	nocopy_ctx *c = &NOCOPY_G(ctx);
	bool copy = !NOCOPY_G(zero_copy);
	nocopy_borrow_begin(c);
	nocopy_entry *e = nocopy_find_ex(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key), stale);
	bool hit = e != NULL;
	ZVAL_NULL(rv);
	if (e) {
		if (exp) {
			*exp = nocopy_entry_expires(e);
			*delta = nocopy_entry_user(e);
		}
		if (nocopy_value_scalar(e, rv)) {
			/* hit */
		} else if (copy) {
			hit = nocopy_kept_read(&NOCOPY_G(kept), &NOCOPY_G(env), e, rv);
		} else {
			int r = nocopy_value_read(&NOCOPY_G(env), e, false, rv);
			hit = r & NOCOPY_HIT;
			if ((r & NOCOPY_BORROWED) && UNEXPECTED(nocopy_keep(c, e))) { /* a pointer into the arena escaped: stay pinned until the request ends */
				NOCOPY_G(zero_copy) = false; /* until the request ends */
			}
		}
	}
	nocopy_count(c, hit);
	nocopy_borrow_end(c);
	return hit;
}

bool nocopy_fetch_local(zend_string *key, zval *rv, time_t *exp, uint32_t *delta) { return local_fetch(key, rv, exp, delta, false); }

static zend_always_inline bool fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_detour->fetch(key, rv, exp, delta);
	}
	return local_fetch(key, rv, exp, delta, false);
}

bool nocopy_fetch(zend_string *key, zval *rv) { return fetch(key, rv, NULL, NULL); }

/* noinline: under LTO, inlined into nocopy_get_multiple, copy mode got 14% slower */
__attribute__((noinline)) void nocopy_fetch_multi(HashTable *keys, HashTable *out)
{
	if (UNEXPECTED(nocopy_detour)) {
		nocopy_detour->fetch_multi(keys, out);
		return;
	}
	zval *k, t;
	nocopy_borrow_begin(&NOCOPY_G(ctx)); /* idle pins checked once per call, not per key */
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		if (Z_TYPE_P(k) == IS_STRING && local_fetch(Z_STR_P(k), &t, NULL, NULL, false)) {
			zend_symtable_update(out, Z_STR_P(k), &t);
		}
	} ZEND_HASH_FOREACH_END();
	nocopy_borrow_end(&NOCOPY_G(ctx));
}

typedef struct {
	const nocopy_entry **e;
	size_t n, cap;
	zend_string *prefix;
} keys_walk;

static void keys_collect(const nocopy_entry *e, void *arg)
{
	keys_walk *w = arg;
	if (nocopy_entry_klen(e) < ZSTR_LEN(w->prefix) || memcmp(nocopy_key(e), ZSTR_VAL(w->prefix), ZSTR_LEN(w->prefix)) != 0) {
		return;
	}
	if (w->n == w->cap) {
		w->cap = w->cap ? 2 * w->cap : 256;
		w->e = erealloc(w->e, w->cap * sizeof *w->e);
	}
	w->e[w->n++] = e;
}

static int keys_by_seq(const void *a, const void *b)
{
	uint64_t x = nocopy_entry_version(*(const nocopy_entry *const *) a), y = nocopy_entry_version(*(const nocopy_entry *const *) b);
	return (x > y) - (x < y);
}

void nocopy_list_keys(zend_string *prefix, HashTable *out)
{
	if (!nocopy_attached()) {
		return;
	}
	nocopy_ctx *c = &NOCOPY_G(ctx);
	keys_walk w = {.prefix = prefix};
	nocopy_borrow_begin(c);
	nocopy_each(c, keys_collect, &w);
	if (w.n) {
		qsort(w.e, w.n, sizeof *w.e, keys_by_seq); /* seq: oldest first, to the coarse clock's tick */
	}
	for (size_t i = 0; i < w.n; i++) {
		const nocopy_entry *e = w.e[i];
		zval info;
		array_init_size(&info, 3);
		add_assoc_long(&info, "size", (zend_long) nocopy_entry_size(e));
		add_assoc_long(&info, "created", (zend_long) nocopy_entry_created(e));
		add_assoc_long(&info, "expires", (zend_long) nocopy_entry_expires(e));
		zend_symtable_str_update(out, nocopy_key(e), nocopy_entry_klen(e), &info);
	}
	nocopy_borrow_end(c);
	if (w.e) {
		efree(w.e);
	}
}

bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o)
{
	if (UNEXPECTED(nocopy_detour)) {
		return nocopy_detour->store_multi(vals, o);
	}
	return nocopy_store_each(vals, o);
}

bool nocopy_store_each(HashTable *vals, const nocopy_opts *o)
{
	zend_ulong h;
	zend_string *k;
	zval *v;
	bool all = true;
	ZEND_HASH_FOREACH_KEY_VAL(vals, h, k, v) {
		zend_string *key = nocopy_key_of(k, h);
		all &= nocopy_store(key, v, o) == NOCOPY_OK;
		zend_string_release(key);
		if (EG(exception)) {
			break;
		}
	} ZEND_HASH_FOREACH_END();
	return all;
}

/* ---- single-flight: a lease per key hash (in the arena's shard) holds who computes it ---- */

#define LEASE_TTL 30 /* s: a live holder stuck longer than this stops blocking the others */

static int lease_take(zend_ulong h)
{
	if (NOCOPY_G(nleases) == (int) (sizeof NOCOPY_G(leases) / sizeof NOCOPY_G(leases)[0])) {
		return NOCOPY_LEASE_FULL; /* nested too deep: compute without one */
	}
	int r = nocopy_lease_take(h, nocopy_ctx_slot(&NOCOPY_G(ctx)), nocopy_now(), LEASE_TTL);
	if (r == NOCOPY_LEASE_TAKEN) {
		NOCOPY_G(leases)[NOCOPY_G(nleases)++] = h;
	}
	return r;
}

static void lease_drop(void)
{
	nocopy_lease_drop(NOCOPY_G(leases)[--NOCOPY_G(nleases)], nocopy_ctx_slot(&NOCOPY_G(ctx)));
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
	int64_t t0 = nocopy_mono_ms();
	zend_result r = zend_call_function(&fci, &fcc);
	zend_release_fcall_info_cache(&fcc);
	zend_long ttl = zval_get_long(Z_REFVAL(args[1]));
	zval_ptr_dtor(&args[1]);
	if (r == FAILURE || EG(exception) || ttl < 0) {
		return; /* ttl < 0: the callback asks not to store */
	}
	int64_t ms = nocopy_mono_ms() - t0;
	const nocopy_opts as = {.ttl = ttl, .add_only = o->add_only};
	store_as(key, rv, &as, ms >= UINT32_MAX ? UINT32_MAX : (uint32_t) ms);
}

void nocopy_remember(zend_string *key, zval *fn, const nocopy_opts *o, double beta, zval *rv)
{
	time_t exp;
	uint32_t delta;
	if (UNEXPECTED(nocopy_detour) && !nocopy_detour->leases) {
		compute(key, fn, o, rv);
		return;
	}
	if (fetch(key, rv, &exp, &delta)) {
		if (!(beta > 0 && exp && delta)) {
			return;
		}
		double r = ((nocopy_rand(&NOCOPY_G(ctx)) >> 8) + 1) / 16777216.0; /* (0, 1] */
		struct timespec ts;
		nocopy_clock(&ts); /* exp's clock */
		if ((double) nocopy_now_of(&ts) + ts.tv_nsec / 1e9 - delta / 1000.0 * beta * log(r) < (double) exp + 1) { /* expires at the end of second exp */
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
	int64_t deadline = nocopy_mono_ms() + LEASE_TTL * 1000;
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
		if (beta > 0 && !nocopy_detour && local_fetch(key, rv, NULL, NULL, true)) {
			return; /* stale-while-revalidate: the expired value, while the holder computes the new one */
		}
		if (nocopy_mono_ms() > deadline) {
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

/* ---- no arena: the detour where every op misses or fails ---- */

static nocopy_status na_store(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user)
{
	(void) key, (void) o, (void) user;
	return nocopy_value_check(&NOCOPY_G(env), val) ? NOCOPY_NO_ROOM : NOCOPY_REFUSED; /* the checks only: the caller's errors still throw */
}
static bool na_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta) { (void) key, (void) exp, (void) delta; ZVAL_NULL(rv); return false; }
static void na_fetch_multi(HashTable *keys, HashTable *out) { (void) keys, (void) out; }
static bool na_drop(zend_string *key) { (void) key; return false; }
static zend_long na_drop_multi(HashTable *keys) { (void) keys; return 0; }
static bool na_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out) { (void) key, (void) delta, (void) ttl; *out = 0; return false; }
static bool na_cas_long(zend_string *key, zend_long old, zend_long nw) { (void) key, (void) old, (void) nw; return false; }
static void na_clear(void) {}

const nocopy_detour_ops nocopy_no_arena = {
	na_store, na_fetch, na_fetch_multi, nocopy_store_each, na_drop, na_drop_multi, na_add_long, na_cas_long, na_clear, false, "none",
};

void nocopy_leases_drop(void)
{
	while (NOCOPY_G(nleases)) {
		lease_drop(); /* a callback bailed out (fatal, timeout) while holding it */
	}
}
