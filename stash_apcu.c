/* stash: the apcu_* compatibility layer and APCUIterator, on top of stash.c's store/fetch/delete. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "stash_php.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"
#include "ext/pcre/php_pcre.h"
#include "stash_apcu_arginfo.h"

/* ---- snapshots and the iterator ---- */

#define APC_LIST_ACTIVE   0x1
#define APC_LIST_DELETED  0x2
#define APC_ITER_TYPE      (1 << 0)
#define APC_ITER_KEY       (1 << 1)
#define APC_ITER_VALUE     (1 << 2)
#define APC_ITER_NUM_HITS  (1 << 3)
#define APC_ITER_MTIME     (1 << 4)
#define APC_ITER_CTIME     (1 << 5)
#define APC_ITER_DTIME     (1 << 6)
#define APC_ITER_ATIME     (1 << 7)
#define APC_ITER_REFCOUNT  (1 << 8)
#define APC_ITER_MEM_SIZE  (1 << 9)
#define APC_ITER_TTL       (1 << 10)
#define APC_ITER_ALL       0xffffffffL

typedef struct {
	zend_string *key;
	time_t created, expires;
	size_t mem;
	uint64_t seq;
} snap_row;

typedef struct {
	snap_row *rows;
	uint32_t count, pos;
	pcre_cache_entry *pce;
	HashTable *keys; /* explicit key set, or NULL */
	zend_long format, list;
	bool initialized, built, valid;
	zend_string *cur_key;
	zval cur_item;
	zend_object std;
} stash_iter;

static zend_class_entry *iter_ce;
static zend_object_handlers iter_handlers;

#define ITER(o) ((stash_iter *) ((char *) (o) - offsetof(stash_iter, std)))

static void rows_free(stash_iter *it)
{
	for (uint32_t i = 0; i < it->count; i++) {
		zend_string_release(it->rows[i].key);
	}
	efree(it->rows);
	it->rows = NULL;
	it->count = 0;
}

static int row_cmp(const void *a, const void *b)
{
	uint64_t x = ((const snap_row *) a)->seq, y = ((const snap_row *) b)->seq;
	return x < y ? -1 : x > y;
}

static bool row_matches(stash_iter *it, zend_string *key)
{
	if (it->pce) {
		zval rv;
		ZVAL_UNDEF(&rv);
#if PHP_VERSION_ID >= 80400
		php_pcre_match_impl(it->pce, key, &rv, NULL, 0, 0, 0);
#else
		php_pcre_match_impl(it->pce, key, &rv, NULL, 0, 0, 0, 0);
#endif
		bool m = Z_TYPE(rv) == IS_LONG && Z_LVAL(rv) > 0;
		zval_ptr_dtor(&rv);
		return m;
	}
	return !it->keys || zend_symtable_exists(it->keys, key);
}

/* Fresh insertion-ordered snapshot of the live keys that match the iterator's filter. */
static void iter_snapshot(stash_iter *it)
{
	if (it->rows) {
		rows_free(it);
	}
	it->pos = 0;
	if (it->list & APC_LIST_DELETED && !(it->list & APC_LIST_ACTIVE)) {
		return;
	}
	/* no lock: emalloc may bail out (OOM) and must never do so with the arena locked */
	uint32_t c = 0, cap = 64;
	snap_row *rows = safe_emalloc(cap, sizeof(snap_row), 0);
	stash_ctx *ctx = &STASH_G(ctx);
	stash_borrow_begin(ctx);
	stash_entry *e;
	for (uint32_t sh = 0, i = 0; (e = stash_scan_next(ctx, &sh, &i));) {
		if (c == cap) {
			cap *= 2;
			rows = safe_erealloc(rows, cap, sizeof(snap_row), 0);
		}
		rows[c].key = zend_string_init(ZSTR_VAL(entry_key(e)), ZSTR_LEN(entry_key(e)), 0);
		rows[c].created = e->created;
		rows[c].expires = e->expires;
		rows[c].mem = e->size;
		rows[c].seq = e->seq;
		c++;
	}
	stash_borrow_end(ctx);

	qsort(rows, c, sizeof(snap_row), row_cmp);
	uint32_t keep = 0;
	for (uint32_t i = 0; i < c; i++) {
		if (row_matches(it, rows[i].key)) {
			rows[keep++] = rows[i];
		} else {
			zend_string_release(rows[i].key);
		}
	}
	it->rows = rows;
	it->count = keep;
}

/* Load the item at/after pos; entries deleted since the snapshot are skipped when a value is wanted. */
static void iter_load(stash_iter *it)
{
	zval_ptr_dtor(&it->cur_item);
	ZVAL_UNDEF(&it->cur_item);
	if (it->cur_key) {
		zend_string_release(it->cur_key);
		it->cur_key = NULL;
	}
	it->valid = false;

	while (it->pos < it->count) {
		snap_row *r = &it->rows[it->pos];
		zval value;
		ZVAL_UNDEF(&value);
		if ((it->format & APC_ITER_VALUE) && !stash_fetch(r->key, &value)) {
			it->pos++;
			continue;
		}
		zval item;
		array_init(&item);
		zend_long f = it->format;
		if (f & APC_ITER_TYPE) add_assoc_string(&item, "type", "user");
		if (f & APC_ITER_KEY) add_assoc_str(&item, "key", zend_string_copy(r->key));
		if (f & APC_ITER_VALUE) add_assoc_zval(&item, "value", &value);
		if (f & APC_ITER_NUM_HITS) add_assoc_long(&item, "num_hits", 0);
		if (f & APC_ITER_MTIME) add_assoc_long(&item, "mtime", r->created);
		if (f & APC_ITER_CTIME) add_assoc_long(&item, "creation_time", r->created);
		if (f & APC_ITER_DTIME) add_assoc_long(&item, "deletion_time", 0);
		if (f & APC_ITER_ATIME) add_assoc_long(&item, "access_time", r->created);
		if (f & APC_ITER_REFCOUNT) add_assoc_long(&item, "ref_count", 0);
		if (f & APC_ITER_MEM_SIZE) add_assoc_long(&item, "mem_size", r->mem);
		if (f & APC_ITER_TTL) add_assoc_long(&item, "ttl", r->expires ? r->expires - r->created : 0);
		ZVAL_COPY_VALUE(&it->cur_item, &item);
		it->cur_key = zend_string_copy(r->key);
		it->valid = true;
		return;
	}
}

static zend_object *iter_create(zend_class_entry *ce)
{
	stash_iter *it = zend_object_alloc(sizeof(stash_iter), ce);
	zend_object_std_init(&it->std, ce);
	object_properties_init(&it->std, ce);
	ZVAL_UNDEF(&it->cur_item);
	it->std.handlers = &iter_handlers;
	return &it->std;
}

static void iter_free(zend_object *o)
{
	stash_iter *it = ITER(o);
	if (it->rows) {
		rows_free(it);
	}
	if (it->keys) {
		zend_array_destroy(it->keys);
	}
	if (it->cur_key) {
		zend_string_release(it->cur_key);
	}
	zval_ptr_dtor(&it->cur_item);
	zend_object_std_dtor(o);
}

/* The iterator for $this, snapshotted on first use. NULL (with an Error thrown) if uninitialized. */
static stash_iter *iter_this(zval *self)
{
	stash_iter *it = ITER(Z_OBJ_P(self));
	if (!it->initialized) {
		zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
		return NULL;
	}
	if (!it->built) {
		iter_snapshot(it);
		iter_load(it);
		it->built = true;
	}
	return it;
}

ZEND_METHOD(APCUIterator, __construct)
{
	zval *search = NULL;
	zend_long format = APC_ITER_ALL, chunk = 0, list = APC_LIST_ACTIVE;

	ZEND_PARSE_PARAMETERS_START(0, 4)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(search)
		Z_PARAM_LONG(format)
		Z_PARAM_LONG(chunk)
		Z_PARAM_LONG(list)
	ZEND_PARSE_PARAMETERS_END();
	(void) chunk;

	if (!stash_apc_on) {
		zend_throw_error(NULL, "APC must be enabled to use APCUIterator");
		RETURN_THROWS();
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	stash_iter *it = ITER(Z_OBJ_P(ZEND_THIS));
	if (search && Z_TYPE_P(search) == IS_STRING) {
		if (Z_STRLEN_P(search)) {
			it->pce = pcre_get_compiled_regex_cache(Z_STR_P(search));
			if (!it->pce) {
				RETURN_THROWS(); /* PCRE already warned; iterator stays uninitialized */
			}
		}
	} else if (search && Z_TYPE_P(search) == IS_ARRAY) {
		zval yes, *v;
		ZVAL_TRUE(&yes);
		it->keys = zend_new_array(zend_hash_num_elements(Z_ARRVAL_P(search)));
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(search), v) {
			ZVAL_DEREF(v);
			if (Z_TYPE_P(v) == IS_STRING) {
				zend_symtable_update(it->keys, Z_STR_P(v), &yes);
			}
		} ZEND_HASH_FOREACH_END();
	} else if (search && Z_TYPE_P(search) != IS_NULL) {
		zend_argument_type_error(1, "must be of type array|string|null, %s given", zend_zval_type_name(search));
		RETURN_THROWS();
	}
	it->format = format;
	it->list = list;
	it->initialized = true;
}

ZEND_METHOD(APCUIterator, rewind)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = ITER(Z_OBJ_P(ZEND_THIS));
	if (!it->initialized) {
		zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
		RETURN_THROWS();
	}
	iter_snapshot(it);
	iter_load(it);
	it->built = true;
}

ZEND_METHOD(APCUIterator, next)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	if (it->valid) {
		it->pos++;
		iter_load(it);
	}
}

ZEND_METHOD(APCUIterator, valid)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_BOOL(it->valid);
}

ZEND_METHOD(APCUIterator, key)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	if (!it->valid) {
		zend_throw_error(NULL, "Cannot call key() on invalid iterator");
		RETURN_THROWS();
	}
	RETURN_STR_COPY(it->cur_key);
}

ZEND_METHOD(APCUIterator, current)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	if (!it->valid) {
		zend_throw_error(NULL, "Cannot call current() on invalid iterator");
		RETURN_THROWS();
	}
	RETURN_COPY(&it->cur_item);
}

/* ponytail: per-entry hit counts are not tracked (would put a shared write on every read) */
ZEND_METHOD(APCUIterator, getTotalHits)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_LONG(0);
}

ZEND_METHOD(APCUIterator, getTotalSize)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	zend_long total = 0;
	for (uint32_t i = 0; i < it->count; i++) {
		total += it->rows[i].mem;
	}
	RETURN_LONG(total);
}

ZEND_METHOD(APCUIterator, getTotalCount)
{
	ZEND_PARSE_PARAMETERS_NONE();
	stash_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_LONG(it->count);
}


/* ---- apcu_* ---- */

#define APC_WARN_KEYS "apc_delete() expects a string, array of strings, or APCIterator instance"

/* String form of a scalar key argument; caller releases. */
static zend_string *key_str(zval *z)
{
	ZVAL_DEREF(z);
	return zval_get_string(z);
}

static void apcu_store_impl(INTERNAL_FUNCTION_PARAMETERS, bool add_only)
{
	zval *key, *val = NULL;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ZVAL(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(val)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!val) {
			zend_wrong_parameters_count_error(2, 3);
			RETURN_THROWS();
		}
		if (!stash_apc_on) {
			RETURN_FALSE;
		}
		if (!stash_tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool ok = stash_store(ks, val, ttl, add_only, true);
		zend_string_release(ks);
		if (EG(exception)) {
			RETURN_THROWS();
		}
		RETURN_BOOL(ok);
	}

	/* array form: returns the keys that failed, each mapped to -1 */
	if (stash_apc_on && !stash_tracked()) {
		RETURN_THROWS();
	}
	array_init(return_value);
	zend_ulong h;
	zend_string *k;
	zval *v;
	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(key), h, k, v) {
		zend_string *ks = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		bool ok = stash_apc_on && stash_store(ks, v, ttl, add_only, true);
		if (EG(exception)) {
			zend_string_release(ks);
			break;
		}
		if (!ok) {
			add_assoc_long_ex(return_value, ZSTR_VAL(ks), ZSTR_LEN(ks), -1);
		}
		zend_string_release(ks);
	} ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(apcu_store) { apcu_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, false); }
PHP_FUNCTION(apcu_add)   { apcu_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, true); }

PHP_FUNCTION(apcu_enabled)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(stash_apc_on);
}

PHP_FUNCTION(apcu_fetch)
{
	zval *key, *success = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ZVAL(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(success)
	ZEND_PARSE_PARAMETERS_END();

	if (success) {
		ZEND_TRY_ASSIGN_REF_FALSE(success);
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!stash_apc_on) {
			RETURN_FALSE;
		}
		if (!stash_tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool hit = stash_fetch(ks, return_value);
		zend_string_release(ks);
		if (!hit) {
			RETVAL_FALSE;
		} else if (success) {
			ZEND_TRY_ASSIGN_REF_TRUE(success);
		}
		return;
	}

	array_init(return_value);
	if (!stash_apc_on) {
		return;
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	zval *v;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(key), v) {
		ZVAL_DEREF(v);
		zval t;
		if (Z_TYPE_P(v) == IS_STRING && stash_fetch(Z_STR_P(v), &t)) {
			zend_symtable_update(Z_ARRVAL_P(return_value), Z_STR_P(v), &t);
		}
	} ZEND_HASH_FOREACH_END();
	if (success) {
		ZEND_TRY_ASSIGN_REF_TRUE(success);
	}
}

static bool exists(zend_string *key)
{
	stash_ctx *c = &STASH_G(ctx);
	stash_borrow_begin(c);
	bool r = stash_find(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key)) != NULL;
	stash_borrow_end(c);
	return r;
}

PHP_FUNCTION(apcu_exists)
{
	zval *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(key)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!stash_apc_on) {
			RETURN_FALSE;
		}
		if (!stash_tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool r = exists(ks);
		zend_string_release(ks);
		RETURN_BOOL(r);
	}
	array_init(return_value);
	if (!stash_apc_on) {
		return;
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	zval *v, yes;
	ZVAL_TRUE(&yes);
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(key), v) {
		ZVAL_DEREF(v);
		if (Z_TYPE_P(v) == IS_STRING && exists(Z_STR_P(v))) {
			zend_symtable_update(Z_ARRVAL_P(return_value), Z_STR_P(v), &yes);
		}
	} ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(apcu_delete)
{
	zval *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(key)
	ZEND_PARSE_PARAMETERS_END();

	if (Z_TYPE_P(key) == IS_OBJECT && instanceof_function(Z_OBJCE_P(key), iter_ce)) {
		stash_iter *it = ITER(Z_OBJ_P(key));
		if (!it->initialized) {
			zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
			RETURN_THROWS();
		}
		iter_snapshot(it);
		for (uint32_t i = 0; i < it->count; i++) {
			stash_drop(it->rows[i].key);
		}
		rows_free(it);
		it->built = false;
		RETURN_TRUE;
	}
	if (Z_TYPE_P(key) == IS_STRING) {
		RETURN_BOOL(stash_apc_on && stash_drop(Z_STR_P(key)));
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		php_error_docref(NULL, E_WARNING, APC_WARN_KEYS);
		RETURN_FALSE;
	}
	array_init(return_value); /* keys that could not be deleted */
	zval *v;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(key), v) {
		ZVAL_DEREF(v);
		if (Z_TYPE_P(v) != IS_STRING) {
			php_error_docref(NULL, E_WARNING, APC_WARN_KEYS);
		}
		if (Z_TYPE_P(v) != IS_STRING || !stash_apc_on || !stash_drop(Z_STR_P(v))) {
			Z_TRY_ADDREF_P(v);
			add_next_index_zval(return_value, v);
		}
	} ZEND_HASH_FOREACH_END();
}

static void incdec(INTERNAL_FUNCTION_PARAMETERS, int sign)
{
	zend_string *key;
	zend_long step = 1, ttl = 0;
	zval *success = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 4)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(step)
		Z_PARAM_ZVAL(success)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (success) {
		ZEND_TRY_ASSIGN_REF_FALSE(success);
	}
	if (!stash_apc_on) {
		RETURN_FALSE;
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	zend_ulong h = zend_string_hash_val(key);
	stash_shard *sh = stash_shard_of(h);
	for (;;) {
		stash_lock(sh);
		stash_entry *e = stash_lookup(sh, h, ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
		if (e && !stash_expired(e)) {
			if (Z_TYPE_P(entry_val(e)) != IS_LONG || (e->flags & ENTRY_SERIALIZED)) {
				stash_unlock(sh);
				RETURN_FALSE;
			}
			/* in place and atomic: lock-free readers copy this word */
			zend_long nv = __atomic_add_fetch(&Z_LVAL_P(entry_val(e)), sign * step, __ATOMIC_RELAXED);
			stash_unlock(sh);
			if (success) {
				ZEND_TRY_ASSIGN_REF_TRUE(success);
			}
			RETURN_LONG(nv);
		}
		stash_unlock(sh);

		zval init;
		ZVAL_LONG(&init, sign * step);
		bool locked;
		stash_entry *n = stash_build(key, &init, ttl, false, &locked);
		if (!n) {
			RETURN_FALSE;
		}
		stash_put_result r = stash_publish(n, true, locked);
		if (r == PUT_OK) {
			if (success) {
				ZEND_TRY_ASSIGN_REF_TRUE(success);
			}
			RETURN_LONG(sign * step);
		}
		if (r == PUT_FULL) {
			RETURN_FALSE;
		}
		/* PUT_EXISTS: someone created it first, go around and update theirs */
	}
}

PHP_FUNCTION(apcu_inc) { incdec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }
PHP_FUNCTION(apcu_dec) { incdec(INTERNAL_FUNCTION_PARAM_PASSTHRU, -1); }

PHP_FUNCTION(apcu_cas)
{
	zend_string *key;
	zend_long old, nw;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_LONG(old)
		Z_PARAM_LONG(nw)
	ZEND_PARSE_PARAMETERS_END();

	if (!stash_apc_on) {
		RETURN_FALSE;
	}
	zend_ulong h = zend_string_hash_val(key);
	stash_shard *sh = stash_shard_of(h);
	stash_lock(sh);
	stash_entry *e = stash_lookup(sh, h, ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	bool ok = e && !stash_expired(e) && !(e->flags & ENTRY_SERIALIZED) && Z_TYPE_P(entry_val(e)) == IS_LONG && Z_LVAL_P(entry_val(e)) == old;
	if (ok) {
		__atomic_store_n(&Z_LVAL_P(entry_val(e)), nw, __ATOMIC_RELAXED);
	}
	stash_unlock(sh);
	RETURN_BOOL(ok);
}

PHP_FUNCTION(apcu_clear_cache)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (!stash_apc_on) {
		RETURN_TRUE;
	}
	stash_clear();
	RETURN_TRUE;
}

/* ponytail: not single-flight, two workers may both run $callback; milestone 3 */
PHP_FUNCTION(apcu_entry)
{
	zend_string *key;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(key)
		Z_PARAM_FUNC(fci, fcc)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (!stash_apc_on) {
		RETURN_NULL();
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	stash_remember(key, &fci, &fcc, ttl, 0, true, return_value);
	if (EG(exception)) {
		RETURN_THROWS();
	}
}

PHP_FUNCTION(apcu_key_info)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	if (!stash_apc_on) {
		RETURN_NULL();
	}
	if (!stash_tracked()) {
		RETURN_THROWS();
	}
	stash_ctx *c = &STASH_G(ctx);
	stash_borrow_begin(c);
	stash_entry *e = stash_find(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	if (!e) {
		stash_borrow_end(c);
		RETURN_NULL();
	}
	array_init(return_value);
	add_assoc_long(return_value, "hits", 0);
	add_assoc_long(return_value, "access_time", e->created);
	add_assoc_long(return_value, "mtime", e->created);
	add_assoc_long(return_value, "creation_time", e->created);
	add_assoc_long(return_value, "deletion_time", 0);
	add_assoc_long(return_value, "ttl", e->expires ? e->expires - e->created : 0);
	add_assoc_long(return_value, "refs", 0);
	stash_borrow_end(c);
}

PHP_FUNCTION(apcu_cache_info)
{
	bool limited = false;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(limited)
	ZEND_PARSE_PARAMETERS_END();

	if (!stash_apc_on) {
		php_error_docref(NULL, E_WARNING, "No APC info available.  Perhaps APC is not enabled? Check apc.enabled in your ini file");
		RETURN_FALSE;
	}

	zval list;
	array_init(&list);
	zend_long entries = 0;

	stash_ctx *c = &STASH_G(ctx);
	stash_borrow_begin(c); /* pins, not the lock: building the list allocates */
	stash_entry *e;
	for (uint32_t sh = 0, i = 0; (e = stash_scan_next(c, &sh, &i));) {
		entries++;
		if (!limited) {
			zval row;
			array_init(&row);
			add_assoc_str(&row, "info", zend_string_init(ZSTR_VAL(entry_key(e)), ZSTR_LEN(entry_key(e)), 0));
			add_assoc_long(&row, "ttl", e->expires ? e->expires - e->created : 0);
			add_assoc_long(&row, "num_hits", 0);
			add_assoc_long(&row, "mtime", e->created);
			add_assoc_long(&row, "creation_time", e->created);
			add_assoc_long(&row, "deletion_time", 0);
			add_assoc_long(&row, "access_time", e->created);
			add_assoc_long(&row, "ref_count", 0);
			add_assoc_long(&row, "mem_size", e->size);
			add_next_index_zval(&list, &row);
		}
	}
	stash_borrow_end(c);
	stash_stats_t st;
	stash_stats(&st);
	array_init(return_value);
	add_assoc_long(return_value, "num_slots", st.slots);
	add_assoc_long(return_value, "ttl", stash_ini_ttl);
	add_assoc_long(return_value, "num_hits", st.hits);
	add_assoc_long(return_value, "num_misses", st.misses);
	add_assoc_long(return_value, "num_inserts", __atomic_load_n(&stash_a->inserts, __ATOMIC_RELAXED));
	add_assoc_long(return_value, "num_entries", entries);
	add_assoc_long(return_value, "expunges", st.expunges);
	add_assoc_long(return_value, "start_time", stash_a->start_time);
	add_assoc_long(return_value, "mem_size", st.used);
	add_assoc_string(return_value, "memory_type", "mmap");
	if (limited) {
		zval_ptr_dtor(&list);
	} else {
		add_assoc_zval(return_value, "cache_list", &list);
		zval deleted;
		array_init(&deleted);
		add_assoc_zval(return_value, "deleted_list", &deleted);
	}
}

PHP_FUNCTION(apcu_sma_info)
{
	bool limited = false;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(limited)
	ZEND_PARSE_PARAMETERS_END();

	if (!stash_apc_on) {
		php_error_docref(NULL, E_WARNING, "No APC SMA info available.  Perhaps APC is disabled via apc.enabled?");
		RETURN_FALSE;
	}

	stash_stats_t st;
	stash_stats(&st);
	uint32_t nsh = stash_a->nshards;

	/* one APCu "segment" per shard: an entry only fits into free space of its own shard's slice */
	array_init(return_value);
	add_assoc_long(return_value, "num_seg", nsh);
	add_assoc_double(return_value, "seg_size", (double) ((size_t) (stash_a->nsegs / nsh) << stash_seg_shift));
	add_assoc_double(return_value, "avail_mem", (double) (((size_t) stash_a->nsegs << stash_seg_shift) - st.used));
	if (limited) {
		return;
	}
	zval outer;
	array_init(&outer);
	for (uint32_t s = 0; s < nsh; s++) {
		size_t run_at[STASH_MAX_SEGS], run_len[STASH_MAX_SEGS];
		uint32_t runs = stash_free_runs(s, run_at, run_len, STASH_MAX_SEGS);
		zval blocks;
		array_init(&blocks);
		for (uint32_t r = 0; r < runs; r++) {
			zval row;
			array_init(&row);
			add_assoc_long(&row, "size", (zend_long) run_len[r]);
			add_assoc_long(&row, "offset", (zend_long) run_at[r]);
			add_next_index_zval(&blocks, &row);
		}
		add_next_index_zval(&outer, &blocks);
	}
	add_assoc_zval(return_value, "block_lists", &outer);
}

void stash_apcu_minit(int module_number)
{
	zend_register_functions(NULL, ext_functions, NULL, MODULE_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_LIST_ACTIVE", APC_LIST_ACTIVE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_LIST_DELETED", APC_LIST_DELETED, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_TYPE", APC_ITER_TYPE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_KEY", APC_ITER_KEY, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_VALUE", APC_ITER_VALUE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_NUM_HITS", APC_ITER_NUM_HITS, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_MTIME", APC_ITER_MTIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_CTIME", APC_ITER_CTIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_DTIME", APC_ITER_DTIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_ATIME", APC_ITER_ATIME, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_REFCOUNT", APC_ITER_REFCOUNT, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_MEM_SIZE", APC_ITER_MEM_SIZE, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_TTL", APC_ITER_TTL, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_NONE", 0, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("APC_ITER_ALL", APC_ITER_ALL, CONST_PERSISTENT);

	iter_ce = register_class_APCUIterator(zend_ce_iterator);
	iter_ce->create_object = iter_create;
	memcpy(&iter_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	iter_handlers.offset = offsetof(stash_iter, std);
	iter_handlers.free_obj = iter_free;
	iter_handlers.clone_obj = NULL;
}
