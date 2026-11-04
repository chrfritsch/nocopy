/* stash: zero-copy shared-memory store with an apcu_* compatibility layer. This file: values in and out of the arena, stash_*, the module. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "stash_php.h"
#include "stash_arena.c" /* one unit with the write path: alloc and commit inline into it (measured ~3 ns per write) */
#include "ext/standard/info.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "SAPI.h"
#include "zend_interfaces.h"
#include "stash_arginfo.h"
#include <pthread.h>
#include <math.h>
#include <unistd.h>

#define STASH_MAX_DEPTH 64
#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)
#define STASH_INLINE_PUT 2048 /* blocks up to this size are filled under the lock: one lock round trip per write */

ZEND_DECLARE_MODULE_GLOBALS(stash)

static size_t arena_size = 32 * 1024 * 1024;
static zend_long ini_hint = 4096;
zend_long stash_ini_ttl;
static bool ini_enabled = true, ini_enable_cli, request_scope, apcu_compat = true;
bool stash_apc_on; /* apc.enabled, and apc.enable_cli under the CLI SAPI */
static zif_handler orig_handle_request; /* FrankenPHP's frankenphp_handle_request, wrapped */

bool stash_tracked(void)
{
	if (STASH_G(ctx).slot < 0) {
		zend_throw_error(NULL, "stash: no free worker slot");
		return false;
	}
	return true;
}

/* ---- sizing and persisting ---- */

enum { WHY_NONE = 0, WHY_FALLBACK, WHY_RESOURCE };

static bool calc_zval(zval *z, int depth, size_t *n, int *why);

/* Empties the key table for the next value. */
static zend_always_inline void keys_reset(void)
{
	if (UNEXPECTED(++STASH_G(kgen) == 0)) {
		memset(STASH_G(keys), 0, STASH_KEYS * sizeof(stash_kslot));
		STASH_G(kgen) = 1;
	}
}

static zend_always_inline stash_kslot *key_slot(zend_ulong h)
{
	return &STASH_G(keys)[h & (STASH_KEYS - 1)];
}

/* An equal key was persisted earlier in this value. Depends only on the key sequence, so calc, persist and relocate agree. */
static zend_always_inline bool key_hit(const stash_kslot *s, const zend_string *k, zend_ulong h)
{
	return s->gen == STASH_G(kgen) && s->h == h && zend_string_equals(s->s, k);
}

static zend_always_inline void key_put(stash_kslot *s, const zend_string *k, zend_ulong h)
{
	s->s = k;
	s->h = h;
	s->gen = STASH_G(kgen);
}

static bool calc_array(zend_array *a, int depth, size_t *n, int *why)
{
	if (zend_hash_num_elements(a) == 0) {
		return true;
	}
	*n += SALIGN(sizeof(zend_array)) + SALIGN(HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : HT_USED_SIZE(a));
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if (!calc_zval(v, depth + 1, n, why)) {
				return false;
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key) {
				stash_kslot *s = key_slot(b->h);
				if (!key_hit(s, b->key, b->h)) {
					*n += SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(b->key)));
					key_put(s, b->key, b->h);
				}
			}
			if (!calc_zval(&b->val, depth + 1, n, why)) {
				return false;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return true;
}

/* Adds the out-of-line bytes persist_zval() will consume.
 * why != NULL: refused values set *why so the caller can fall back or warn; else they throw. */
static bool calc_zval(zval *z, int depth, size_t *n, int *why)
{
	if (depth > STASH_MAX_DEPTH) {
		if (why) {
			*why = WHY_FALLBACK;
		} else {
			zend_value_error("stash: value is nested too deeply");
		}
		return false;
	}
	switch (Z_TYPE_P(z)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE:
			return true;
		case IS_REFERENCE:
			if (why && Z_REFCOUNT_P(z) > 1) { /* a shared reference must stay shared: serialize */
				*why = WHY_FALLBACK;
				return false;
			}
			return calc_zval(Z_REFVAL_P(z), depth + 1, n, why);
		case IS_STRING:
			*n += SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN_P(z)));
			return true;
		case IS_ARRAY:
			return calc_array(Z_ARR_P(z), depth, n, why);
		default:
			if (why) {
				*why = Z_TYPE_P(z) == IS_RESOURCE ? WHY_RESOURCE : WHY_FALLBACK;
			} else {
				zend_argument_type_error(2, "must not contain objects or resources");
			}
			return false;
	}
}

static void *take(size_t n)
{
	void *p = STASH_G(cursor);
	STASH_G(cursor) += SALIGN(n);
	return p;
}

static zend_string *persist_str(const zend_string *s)
{
	size_t size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(s));
	zend_string *p = take(size);
	memcpy(p, s, size); /* keys arrive hashed; values stay unhashed until used as a key (benign idempotent write) */
	GC_SET_REFCOUNT(p, 2);
	/* not IS_STR_PERMANENT: that promises process lifetime (PCRE then keeps the pointer as a cache key) */
	GC_TYPE_INFO(p) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	return p;
}

static void persist_zval(zval *z);

static zend_array *persist_array(const zend_array *src)
{
	if (zend_hash_num_elements(src) == 0) {
		return (zend_array *) &zend_empty_array;
	}
	zend_array *ht = take(sizeof(zend_array));
	memcpy(ht, src, sizeof(zend_array));

	size_t size = HT_IS_PACKED(src) ? HT_PACKED_USED_SIZE(src) : HT_USED_SIZE(src);
	void *data = take(size);
	memcpy(data, HT_GET_DATA_ADDR(src), size);
	HT_SET_DATA_ADDR(ht, data);

	ht->nInternalPointer = 0;
	GC_SET_REFCOUNT(ht, 2);
	GC_TYPE_INFO(ht) = GC_ARRAY | IS_ARRAY_IMMUTABLE;

	if (HT_IS_PACKED(ht)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(ht, v) {
			persist_zval(v);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(ht, b) {
			if (b->key) {
				stash_kslot *s = key_slot(b->h);
				if (key_hit(s, b->key, b->h)) {
					b->key = (zend_string *) s->s;
				} else {
					b->key = persist_str(b->key);
					key_put(s, b->key, b->h);
				}
			}
			persist_zval(&b->val);
		} ZEND_HASH_FOREACH_END();
	}

	/* serialization does not keep the next free index, so neither do we: max int key + 1 */
	zend_long max = ZEND_LONG_MIN;
	zend_ulong ih;
	zend_string *ik;
	ZEND_HASH_FOREACH_KEY(ht, ih, ik) {
		if (!ik && (zend_long) ih > max) {
			max = (zend_long) ih;
		}
	} ZEND_HASH_FOREACH_END();
	ht->nNextFreeElement = (max == ZEND_LONG_MIN || max == ZEND_LONG_MAX) ? max : max + 1;
	return ht;
}

/* Rewrites *z in place so everything it points to lives in the current block. */
static void persist_zval(zval *z)
{
	switch (Z_TYPE_P(z)) {
		case IS_REFERENCE:
			ZVAL_COPY_VALUE(z, Z_REFVAL_P(z));
			persist_zval(z);
			break;
		case IS_STRING:
			Z_STR_P(z) = persist_str(Z_STR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		case IS_ARRAY:
			Z_ARR_P(z) = persist_array(Z_ARR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		default: /* scalars: calc_zval() already rejected everything else */
			break;
	}
}

/* Fills e after its header: the key string (at koff), then the value. */
static void persist_entry(stash_entry *e, const zend_string *key, zval *val)
{
	STASH_G(cursor) = (char *) e + SALIGN(sizeof(php_entry));
	zend_string *k = persist_str(key);
	e->koff = (uint8_t) (ZSTR_VAL(k) - (char *) e);
	ZVAL_COPY_VALUE(entry_val(e), val);
	if (Z_TYPE_P(val) == IS_ARRAY) {
		keys_reset();
	}
	persist_zval(entry_val(e));
	ZEND_ASSERT(STASH_G(cursor) == (char *) e + e->size);
}

/* Called by the arena when a merge moves an entry; the shard is locked, nothing here allocates. */
void stash_relocate(stash_entry *dst, const stash_entry *src)
{
	persist_entry(dst, entry_key(src), &((php_entry *) src)->val);
}

/* ---- leaving shared memory: deep copies ---- */

static bool in_arena(const void *p)
{
	return (uintptr_t) p - (uintptr_t) stash_a < arena_size;
}

static void unshare_zval(zval *z, int depth);
static void unshare_contents(zend_array *a, int depth);

/* Deep copy of an arena value into ordinary request memory. */
static void copy_out(zval *dst, const zval *src)
{
	switch (Z_TYPE_P(src)) {
		case IS_STRING:
			ZVAL_STRINGL(dst, Z_STRVAL_P(src), Z_STRLEN_P(src));
			break;
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(src);
			if (zend_hash_num_elements(a) == 0) {
				ZVAL_EMPTY_ARRAY(dst);
				break;
			}
			/* bulk-copy the buckets, then repoint whatever still lives in the arena */
			ZVAL_ARR(dst, zend_array_dup(a));
			unshare_contents(Z_ARR_P(dst), 0);
			break;
		}
		default:
			ZVAL_COPY_VALUE(dst, src);
	}
}

/* Repoints every key and value of a (private) array that still lives in the arena. */
static void unshare_contents(zend_array *a, int depth)
{
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			unshare_zval(v, depth + 1);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key && in_arena(b->key)) {
				stash_kslot *s = key_slot(b->h);
				if (s->gen == STASH_G(kgen) && s->s == b->key) { /* a deduped key: share its copy */
					b->key = zend_string_copy(s->copy);
				} else {
					key_put(s, b->key, b->h);
					b->key = s->copy = zend_string_init(ZSTR_VAL(b->key), ZSTR_LEN(b->key), 0);
					ZSTR_H(b->key) = b->h;
				}
				HT_FLAGS(a) &= ~HASH_FLAG_STATIC_KEYS;
			}
			unshare_zval(&b->val, depth + 1);
		} ZEND_HASH_FOREACH_END();
	}
}

/* Replaces an arena string or array inside a copied-out array with a private copy. */
static void unshare_zval(zval *z, int depth)
{
	if (depth > STASH_MAX_DEPTH) {
		zend_value_error("stash: value is nested too deeply");
		return;
	}
	switch (Z_TYPE_P(z)) {
		case IS_REFERENCE:
			unshare_zval(Z_REFVAL_P(z), depth + 1);
			break;
		case IS_STRING:
			if (in_arena(Z_STR_P(z))) {
				zval t;
				copy_out(&t, z);
				ZVAL_COPY_VALUE(z, &t);
			}
			break;
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(z);
			if (in_arena(a)) {
				zval t;
				copy_out(&t, z);
				ZVAL_COPY_VALUE(z, &t);
			}
			break;
		}
	}
}

/* ---- core operations ---- */

stash_entry *stash_build(zend_string *key, zval *val, zend_long ttl, bool apcu_semantics, bool *locked)
{
	*locked = false;
	zval ser;
	bool serialized = false;
	size_t payload = SALIGN(sizeof(php_entry)) + SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(key)));
	size_t body = 0;
	int why = WHY_NONE;

	if (!ttl) {
		ttl = stash_ini_ttl;
	}
	if (Z_TYPE_P(val) == IS_ARRAY) {
		keys_reset();
	}
	if (!calc_zval(val, 0, &body, apcu_semantics ? &why : NULL)) {
		if (!why) {
			return NULL; /* strict mode already threw */
		}
		if (why == WHY_RESOURCE) {
			php_error_docref(NULL, E_WARNING, "Cannot store resources in apcu cache");
			return NULL;
		}
		smart_str buf = {0};
		BG(serialize_lock)++;
		php_serialize_data_t h = php_var_serialize_init();
		php_var_serialize(&buf, val, &h);
		php_var_serialize_destroy(h);
		BG(serialize_lock)--;
		if (EG(exception)) {
			smart_str_free(&buf);
			return NULL;
		}
		smart_str_0(&buf);
		ZVAL_STR(&ser, buf.s);
		val = &ser;
		serialized = true;
		body = 0;
		calc_zval(val, 0, &body, NULL);
	}
	payload += body;

	zend_ulong h = zend_string_hash_val(key);
	stash_shard *sh = stash_shard_of(h);
	stash_lock(sh);
	time_t now = time(NULL), exp = ttl ? now + ttl : 0; /* inside the lock: measured faster under write contention than before it */
	stash_entry *e = stash_alloc(sh, payload, exp, now);
	*locked = e && payload <= STASH_INLINE_PUT;
	if (!*locked) {
		if (e) {
			stash_wip(&STASH_G(ctx), e); /* filled outside the lock: a death meanwhile is rolled back */
		}
		stash_unlock(sh);
	}

	if (e) {
		e->h = h;
		e->klen = (uint32_t) ZSTR_LEN(key);
		e->expires = exp;
		e->created = now;
		e->flags = serialized ? ENTRY_SERIALIZED : 0;
		e->user = 0;
		persist_entry(e, key, val);
	}
	if (serialized) {
		zend_string_release(Z_STR(ser));
	}
	return e;
}

stash_put_result stash_publish(stash_entry *e, bool add_only, bool locked)
{
	stash_shard *sh = stash_shard_of(e->h);
	if (!locked) {
		stash_lock(sh);
	}
	stash_put_result r = stash_commit(sh, e, add_only);
	if (!locked) {
		stash_wip(&STASH_G(ctx), NULL);
	}
	stash_unlock(sh);
	return r;
}

bool stash_store(zend_string *key, zval *val, zend_long ttl, bool add_only, bool apcu_semantics)
{
	bool locked;
	stash_entry *e = stash_build(key, val, ttl, apcu_semantics, &locked);
	return e && stash_publish(e, add_only, locked) == PUT_OK;
}

bool stash_drop(zend_string *key)
{
	zend_ulong h = zend_string_hash_val(key);
	stash_shard *sh = stash_shard_of(h);
	stash_lock(sh);
	bool found = stash_remove(sh, h, ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	stash_unlock(sh);
	return found;
}

/* Unserialize with a fresh state: php_unserialize_with_options() would share the outer
 * unserialize()'s reference table when called re-entrantly (e.g. from Serializable::unserialize). */
static bool cache_unserialize(zval *rv, const char *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *) buf;
	BG(serialize_lock)++; /* forces a private state instead of the outer call's */
	php_unserialize_data_t h = php_var_unserialize_init();
	bool ok = php_var_unserialize(rv, &p, p + len, &h);
	php_var_unserialize_destroy(h);
	BG(serialize_lock)--;
	if (!ok || EG(exception)) {
		zval_ptr_dtor(rv);
		ZVAL_NULL(rv);
		return false;
	}
	if (Z_ISREF_P(rv)) { /* a top-level cycle comes back as a reference; functions must not return one */
		zend_unwrap_reference(rv);
	}
	return true;
}

/* Fetch into rv: zero-copy in a request-scoped request, else a private copy. exp/delta: the entry's expiry and compute time (ms). */
static zend_always_inline bool fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	stash_ctx *c = &STASH_G(ctx);
	bool copy = !STASH_G(zero_copy);
	stash_borrow_begin(c);
	stash_entry *e = stash_find(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	bool hit = e != NULL;
	ZVAL_NULL(rv);
	if (e) {
		zval *v = entry_val(e);
		if (exp) {
			*exp = e->expires;
			*delta = e->user;
		}
		if (e->flags & ENTRY_SERIALIZED) {
			hit = cache_unserialize(rv, Z_STRVAL_P(v), Z_STRLEN_P(v));
		} else if (copy) {
			if (Z_TYPE_P(v) == IS_ARRAY) {
				keys_reset();
			}
			copy_out(rv, v);
		} else {
			ZVAL_COPY_VALUE(rv, v);
			stash_keep(c, e); /* a pointer into the arena escaped: stay pinned until the request ends */
		}
	}
	uint64_t *ctr = hit ? &stash_a->workers[c->slot].hits : &stash_a->workers[c->slot].misses;
	__atomic_store_n(ctr, *ctr + 1, __ATOMIC_RELAXED); /* single writer per slot: no RMW needed */
	stash_borrow_end(c);
	return hit;
}

bool stash_fetch(zend_string *key, zval *rv) { return fetch(key, rv, NULL, NULL); }

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
	if (STASH_G(nleases) == (int) (sizeof STASH_G(leases) / sizeof STASH_G(leases)[0])) {
		return STASH_LEASE_FULL; /* nested too deep: compute without one */
	}
	int r = stash_lease_take(h, STASH_G(ctx).slot, time(NULL), LEASE_TTL);
	if (r == STASH_LEASE_TAKEN) {
		STASH_G(leases)[STASH_G(nleases)++] = h;
	}
	return r;
}

static void lease_drop(void)
{
	stash_lease_drop(STASH_G(leases)[--STASH_G(nleases)], STASH_G(ctx).slot);
}

/* Runs the callback, measures it, stores the result with its compute time. */
static bool compute(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long ttl, bool apcu, zval *rv)
{
	zval arg;
	ZVAL_STR(&arg, key);
	fci->param_count = 1;
	fci->params = &arg;
	fci->retval = rv;
	double t0 = now_s(CLOCK_MONOTONIC);
	if (zend_call_function(fci, fcc) == FAILURE || EG(exception)) {
		return false;
	}
	double ms = (now_s(CLOCK_MONOTONIC) - t0) * 1000;
	bool locked;
	stash_entry *e = stash_build(key, rv, ttl, true, &locked);
	if (e) {
		e->user = ms >= UINT32_MAX ? UINT32_MAX : (uint32_t) ms;
		stash_publish(e, apcu, locked); /* apcu_entry: add, like APCu */
	}
	return !EG(exception);
}

void stash_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long ttl, double beta, bool apcu, zval *rv)
{
	time_t exp;
	uint32_t delta;
	if (fetch(key, rv, &exp, &delta)) {
		if (!(beta > 0 && exp && delta)) {
			return;
		}
		double r = ((stash_rand(&STASH_G(ctx)) >> 8) + 1) / 16777216.0; /* (0, 1] */
		if (now_s(CLOCK_REALTIME) - delta / 1000.0 * beta * log(r) < (double) exp + 1) { /* expires at the end of second exp */
			return; /* XFetch: not yet */
		}
		if (lease_take(zend_string_hash_val(key)) != STASH_LEASE_TAKEN) {
			return; /* someone else refreshes: keep serving this value */
		}
		zval_ptr_dtor(rv);
		ZVAL_NULL(rv);
		compute(key, fci, fcc, ttl, apcu, rv);
		lease_drop();
		return;
	}
	zend_ulong h = zend_string_hash_val(key);
	double deadline = now_s(CLOCK_MONOTONIC) + LEASE_TTL;
	unsigned wait = 50;
	bool mine = false;
	for (;;) {
		int r = lease_take(h);
		if (r != STASH_LEASE_HELD) {
			mine = r == STASH_LEASE_TAKEN; /* MINE: our own callback asks for its key; FULL: compute without */
			break;
		}
		if (fetch(key, rv, NULL, NULL)) {
			return; /* the holder stored it */
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
	compute(key, fci, fcc, ttl, apcu, rv);
	if (mine) {
		lease_drop();
	}
}

/* ---- stash_* ---- */

PHP_FUNCTION(stash_put)
{
	zend_string *key;
	zval *val;
	zend_long ttl = 0;
	bool serialize = false;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(val)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
		Z_PARAM_BOOL(serialize)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	bool ok = stash_store(key, val, ttl, false, serialize);
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(ok);
}

PHP_FUNCTION(stash_get)
{
	zend_string *key;
	zval *hit = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(hit)
	ZEND_PARSE_PARAMETERS_END();

	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	bool found = stash_fetch(key, return_value);
	if (hit) {
		ZEND_TRY_ASSIGN_REF_BOOL(hit, found);
	}
}

PHP_FUNCTION(stash_delete)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(stash_drop(key));
}

PHP_FUNCTION(stash_remember)
{
	zend_string *key;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	zend_long ttl = 0;
	double beta = 1.0;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(key)
		Z_PARAM_FUNC(fci, fcc)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
		Z_PARAM_DOUBLE(beta)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	stash_remember(key, &fci, &fcc, ttl, beta, false, return_value);
	if (EG(exception)) {
		RETURN_THROWS();
	}
}

PHP_FUNCTION(stash_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_stats_t s;
	stash_stats(&s);
	array_init(return_value);
	add_assoc_long(return_value, "size", arena_size);
	add_assoc_long(return_value, "used", s.used);
	add_assoc_long(return_value, "segment_size", (zend_long) 1 << stash_seg_shift);
	add_assoc_long(return_value, "segments", s.segments);
	add_assoc_long(return_value, "free_segments", s.free_segments);
	add_assoc_long(return_value, "dead_segments", s.dead_segments);
	add_assoc_long(return_value, "pinned_segments", s.pinned_segments);
	add_assoc_long(return_value, "entries", s.entries);
	add_assoc_long(return_value, "hits", s.hits);
	add_assoc_long(return_value, "misses", s.misses);
	add_assoc_long(return_value, "evicted", s.evicted);
	add_assoc_long(return_value, "expired", s.expired);
	add_assoc_long(return_value, "ghost_hits", s.ghost_hits);
	add_assoc_long(return_value, "evacuated_bytes", s.evacuated);
	add_assoc_long(return_value, "shards", s.shards);
	add_assoc_long(return_value, "slots", s.slots);
	add_assoc_long(return_value, "expunges", s.expunges);
	add_assoc_long(return_value, "lock_steals", s.lock_steals);
	add_assoc_long(return_value, "leases", s.leases);
}

/* ---- FrankenPHP worker mode ---- */

/* frankenphp_handle_request() serves many requests inside one worker script, which may keep values across them: copy mode. */
static ZEND_NAMED_FUNCTION(stash_handle_request)
{
	STASH_G(zero_copy) = false; /* values fetched while booting stay pinned until the worker ends */
	orig_handle_request(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* ---- module ---- */

static ZEND_INI_MH(OnBool)
{
	bool v = zend_ini_parse_bool(new_value);
	const char *name = strchr(ZSTR_VAL(entry->name), '.') + 1;
	if (strcmp(name, "enabled") == 0) {
		ini_enabled = v;
	} else if (strcmp(name, "enable_cli") == 0) {
		ini_enable_cli = v;
	} else if (strcmp(name, "apcu_compat") == 0) {
		apcu_compat = v;
	}
	return SUCCESS;
}

/* "auto" (default): zero-copy for the whole request only under SAPIs whose requests are short. */
static ZEND_INI_MH(OnRequestScope)
{
	if (zend_string_equals_literal_ci(new_value, "auto")) {
		static const char *const short_lived[] = {"fpm-fcgi", "cgi-fcgi", "apache2handler", "apache2filter", "litespeed", "fcgi", "cli-server", "frankenphp", NULL};
		request_scope = false;
		for (int i = 0; short_lived[i]; i++) {
			request_scope |= strcmp(sapi_module.name, short_lived[i]) == 0;
		}
	} else {
		request_scope = zend_ini_parse_bool(new_value);
	}
	return SUCCESS;
}

static ZEND_INI_MH(OnShmSize)
{
	arena_size = (size_t) zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnHint)
{
	ini_hint = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnTtl)
{
	stash_ini_ttl = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY("stash.request_scope", "auto", PHP_INI_SYSTEM, OnRequestScope)
	PHP_INI_ENTRY("stash.apcu_compat", "1", PHP_INI_SYSTEM, OnBool)
PHP_INI_END()

static const zend_ini_entry_def ini_stash[] = {
	PHP_INI_ENTRY("stash.shm_size", "32M", PHP_INI_SYSTEM, OnShmSize)
	PHP_INI_ENTRY("stash.entries_hint", "4096", PHP_INI_SYSTEM, OnHint)
	PHP_INI_ENTRY("stash.ttl", "0", PHP_INI_SYSTEM, OnTtl)
	{0}
};

static bool configured(const char *name)
{
	zend_string *n = zend_string_init(name, strlen(name), 0);
	bool set = zend_get_configuration_directive(n) != NULL;
	zend_string_release(n);
	return set;
}

/* MINIT only: set an entry's value (and apply it) without zend_alter_ini_entry's runtime bookkeeping, which a request then undoes. */
static void ini_put(const char *name, zend_string *value, bool apply)
{
	zend_ini_entry *e = zend_hash_str_find_ptr(EG(ini_directives), name, strlen(name));
	if (apply && e->on_modify) {
		e->on_modify(e, value, e->mh_arg1, e->mh_arg2, e->mh_arg3, ZEND_INI_STAGE_STARTUP);
	}
	zend_string *old = e->value;
	e->value = zend_string_copy(value);
	if (old) {
		zend_string_release_ex(old, 1);
	}
}

/* apc.X with the APCu layer: an alias of stash.X, used only when set and stash.X is not. */
static ZEND_INI_MH(OnAlias)
{
	char canon[32];
	snprintf(canon, sizeof canon, "stash.%s", ZSTR_VAL(entry->name) + sizeof("apc.") - 1);
	if (stage != ZEND_INI_STAGE_STARTUP || !configured(ZSTR_VAL(entry->name))) {
		return SUCCESS; /* the default */
	}
	if (configured(canon)) {
		php_error_docref(NULL, E_CORE_WARNING, "stash: %s is ignored, %s is set", ZSTR_VAL(entry->name), canon);
	} else {
		ini_put(canon, new_value, true);
	}
	return SUCCESS;
}

static const zend_ini_entry_def ini_apc[] = {
	PHP_INI_ENTRY("apc.enabled", "1", PHP_INI_SYSTEM, OnBool)
	PHP_INI_ENTRY("apc.enable_cli", "0", PHP_INI_SYSTEM, OnBool)
	PHP_INI_ENTRY("apc.shm_size", "32M", PHP_INI_SYSTEM, OnAlias)
	PHP_INI_ENTRY("apc.entries_hint", "4096", PHP_INI_SYSTEM, OnAlias)
	PHP_INI_ENTRY("apc.ttl", "0", PHP_INI_SYSTEM, OnAlias)
	{0}
};

/* ini_get() of an alias shows what is in effect. */
static void sync_aliases(void)
{
	static const char *const names[] = {"shm_size", "entries_hint", "ttl"};
	for (int i = 0; i < 3; i++) {
		char apc[32], canon[32];
		snprintf(apc, sizeof apc, "apc.%s", names[i]);
		snprintf(canon, sizeof canon, "stash.%s", names[i]);
		ini_put(apc, zend_ini_str(canon, strlen(canon), false), false);
	}
}

static void atfork_child(void)
{
	if (stash_a) {
		stash_after_fork(&STASH_G(ctx));
	}
}

static PHP_GINIT_FUNCTION(stash)
{
#if defined(COMPILE_DL_STASH) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(stash_globals, 0, sizeof(*stash_globals));
	stash_globals->ctx.slot = -1;
	stash_globals->keys = pecalloc(STASH_KEYS, sizeof(stash_kslot), 1);
}

/* A thread ending (ZTS) gives its worker slot back. */
static PHP_GSHUTDOWN_FUNCTION(stash)
{
	if (stash_a) {
		stash_release(&stash_globals->ctx);
	}
	pefree(stash_globals->keys, 1);
}

PHP_MINIT_FUNCTION(stash)
{
	REGISTER_INI_ENTRIES();
	if (apcu_compat && zend_hash_str_exists(&module_registry, ZEND_STRL("apcu"))) {
		php_error_docref(NULL, E_CORE_WARNING, "stash: APCu is loaded, running without the apcu_* layer (stash.apcu_compat=0)");
		apcu_compat = false;
	}
	zend_register_ini_entries(ini_stash, module_number);
	if (apcu_compat) {
		zend_register_ini_entries(ini_apc, module_number);
		sync_aliases();
	}
	stash_apc_on = ini_enabled && (ini_enable_cli || strcmp(sapi_module.name, "cli") != 0);

	size_t need;
	const char *err = stash_arena_create(arena_size, ini_hint > 0 ? (uint64_t) ini_hint : 0, &need);
	const char *p = apcu_compat ? "apc" : "stash";
	if (err == STASH_TOO_SMALL) { /* APCu's wording: its tests and users grep for it */
		zend_error(E_CORE_ERROR, "Unable to allocate %zu bytes of shared memory for cache structures. Either %s.shm_size is too small or %s.entries_hint too large", need, p, p);
		return FAILURE;
	}
	if (err) {
		zend_error(E_CORE_ERROR, "stash: unable to create the %zu byte cache: %s", arena_size, err);
		return FAILURE;
	}
	arena_size = stash_a->size;
	pthread_atfork(NULL, NULL, atfork_child);

	zend_function *hr = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("frankenphp_handle_request"));
	if (hr && hr->type == ZEND_INTERNAL_FUNCTION) {
		orig_handle_request = hr->internal_function.handler;
		hr->internal_function.handler = stash_handle_request;
	}
	if (apcu_compat) {
		stash_apcu_minit(module_number);
	}
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(stash)
{
	stash_release(&STASH_G(ctx));
	stash_arena_destroy();
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(stash)
{
#if defined(COMPILE_DL_STASH) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	if (STASH_G(ctx).slot < 0) {
		stash_claim(&STASH_G(ctx));
	}
	STASH_G(zero_copy) = request_scope;
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(stash)
{
	STASH_G(zero_copy) = false;
	return SUCCESS;
}

/* Pins go only after zend_deactivate: built-in modules (session) still read user values in their RSHUTDOWN, which runs after ours. */
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(stash)
{
	while (STASH_G(nleases)) {
		lease_drop(); /* a callback bailed out (fatal, timeout) while holding it */
	}
	STASH_G(ctx).pin_refs = 0;
	stash_keep_release(&STASH_G(ctx));
	return SUCCESS;
}

PHP_MINFO_FUNCTION(stash)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "stash support", "enabled");
#ifdef ZTS
	php_info_print_table_row(2, "thread safety", "enabled");
#endif
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

zend_module_entry stash_module_entry = {
	STANDARD_MODULE_HEADER,
	"stash",
	ext_functions,
	PHP_MINIT(stash),
	PHP_MSHUTDOWN(stash),
	PHP_RINIT(stash),
	PHP_RSHUTDOWN(stash),
	PHP_MINFO(stash),
	PHP_STASH_VERSION,
	PHP_MODULE_GLOBALS(stash),
	PHP_GINIT(stash),
	PHP_GSHUTDOWN(stash),
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(stash),
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_STASH
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(stash)
#endif
