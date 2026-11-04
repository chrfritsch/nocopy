/* nocopy: the apcu_* compatibility layer and APCUIterator, on top of nocopy.c's store/fetch/delete. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"
#include "ext/pcre/php_pcre.h"
#include "nocopy_apcu_arginfo.h"

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
} nocopy_iter;

static zend_class_entry *iter_ce;
static zend_object_handlers iter_handlers;

#define ITER(o) ((nocopy_iter *) ((char *) (o) - offsetof(nocopy_iter, std)))

static void rows_free(nocopy_iter *it)
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

static bool row_matches(nocopy_iter *it, zend_string *key)
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

typedef struct {
	snap_row *rows;
	uint32_t count, cap;
} snap;

static void snap_add(void *arg, const zend_string *key, const nocopy_meta *m)
{
	snap *s = arg;
	if (s->count == s->cap) {
		s->cap *= 2;
		s->rows = safe_erealloc(s->rows, s->cap, sizeof(snap_row), 0);
	}
	s->rows[s->count++] = (snap_row) {
		.key = zend_string_init(ZSTR_VAL(key), ZSTR_LEN(key), 0), .created = m->created, .expires = m->expires, .mem = m->size, .seq = m->seq,
	};
}

/* Fresh insertion-ordered snapshot of the live keys that match the iterator's filter. */
static void iter_snapshot(nocopy_iter *it)
{
	if (it->rows) {
		rows_free(it);
	}
	it->pos = 0;
	if (it->list & APC_LIST_DELETED && !(it->list & APC_LIST_ACTIVE)) {
		return;
	}
	snap sn = {.rows = safe_emalloc(64, sizeof(snap_row), 0), .cap = 64};
	nocopy_scan(snap_add, &sn);
	snap_row *rows = sn.rows;
	uint32_t c = sn.count;

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
static void iter_load(nocopy_iter *it)
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
		if ((it->format & APC_ITER_VALUE) && !nocopy_fetch(r->key, &value)) {
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
	nocopy_iter *it = zend_object_alloc(sizeof(nocopy_iter), ce);
	zend_object_std_init(&it->std, ce);
	object_properties_init(&it->std, ce);
	ZVAL_UNDEF(&it->cur_item);
	it->std.handlers = &iter_handlers;
	return &it->std;
}

static void iter_free(zend_object *o)
{
	nocopy_iter *it = ITER(o);
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
static nocopy_iter *iter_this(zval *self)
{
	nocopy_iter *it = ITER(Z_OBJ_P(self));
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

	if (!nocopy_apc_on) {
		zend_throw_error(NULL, "APC must be enabled to use APCUIterator");
		RETURN_THROWS();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	nocopy_iter *it = ITER(Z_OBJ_P(ZEND_THIS));
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
	nocopy_iter *it = ITER(Z_OBJ_P(ZEND_THIS));
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
	nocopy_iter *it = iter_this(ZEND_THIS);
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
	nocopy_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_BOOL(it->valid);
}

ZEND_METHOD(APCUIterator, key)
{
	ZEND_PARSE_PARAMETERS_NONE();
	nocopy_iter *it = iter_this(ZEND_THIS);
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
	nocopy_iter *it = iter_this(ZEND_THIS);
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
	nocopy_iter *it = iter_this(ZEND_THIS);
	if (!it) {
		RETURN_THROWS();
	}
	RETURN_LONG(0);
}

ZEND_METHOD(APCUIterator, getTotalSize)
{
	ZEND_PARSE_PARAMETERS_NONE();
	nocopy_iter *it = iter_this(ZEND_THIS);
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
	nocopy_iter *it = iter_this(ZEND_THIS);
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

static void store_impl(zval *key, zval *val, zend_long ttl, bool add_only, zval *return_value)
{
	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .add_only = add_only, .objects = NOCOPY_SERIALIZE_OBJECTS};
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!val) {
			zend_wrong_parameters_count_error(2, 3);
			RETURN_THROWS();
		}
		if (!nocopy_apc_on) {
			RETURN_FALSE;
		}
		if (!nocopy_tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		nocopy_status st = nocopy_store(ks, val, &o);
		zend_string_release(ks);
		if (EG(exception)) {
			RETURN_THROWS();
		}
		RETURN_BOOL(st == NOCOPY_OK);
	}

	/* array form: returns the keys that failed, each mapped to -1 */
	if (nocopy_apc_on && !nocopy_tracked()) {
		RETURN_THROWS();
	}
	array_init(return_value);
	if (nocopy_apc_on) {
		nocopy_store_multi(Z_ARRVAL_P(key), &o, Z_ARRVAL_P(return_value));
		return;
	}
	zend_ulong h;
	zend_string *k;
	ZEND_HASH_FOREACH_KEY(Z_ARRVAL_P(key), h, k) {
		zend_string *ks = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		add_assoc_long_ex(return_value, ZSTR_VAL(ks), ZSTR_LEN(ks), -1);
		zend_string_release(ks);
	} ZEND_HASH_FOREACH_END();
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

	store_impl(key, val, ttl, add_only, return_value);
}

PHP_FUNCTION(apcu_store) { apcu_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, false); }
PHP_FUNCTION(apcu_add)   { apcu_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, true); }

#if PHP_VERSION_ID >= 80400
/* Frameless variants (PHP 8.4+): a call the compiler resolved skips the VM's call frame. */
ZEND_FRAMELESS_FUNCTION(apcu_store, 1) { store_impl(arg1, NULL, 0, false, return_value); } /* must exist: a 1-arg call would pick arity 2, and $value has no default */
ZEND_FRAMELESS_FUNCTION(apcu_store, 2) { store_impl(arg1, arg2, 0, false, return_value); }
ZEND_FRAMELESS_FUNCTION(apcu_store, 3)
{
	zend_long ttl;
	Z_FLF_PARAM_LONG(3, ttl);
	store_impl(arg1, arg2, ttl, false, return_value);
flf_clean:;
}
#endif

PHP_FUNCTION(apcu_enabled)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(nocopy_apc_on);
}

static void fetch_impl(zval *key, zval *success, zval *return_value)
{
	if (success) {
		ZEND_TRY_ASSIGN_REF_FALSE(success);
	}
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!nocopy_apc_on) {
			RETURN_FALSE;
		}
		if (!nocopy_tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool hit = nocopy_fetch(ks, return_value);
		zend_string_release(ks);
		if (!hit) {
			RETVAL_FALSE;
		} else if (success) {
			ZEND_TRY_ASSIGN_REF_TRUE(success);
		}
		return;
	}

	array_init(return_value);
	if (!nocopy_apc_on) {
		return;
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	nocopy_fetch_multi(Z_ARRVAL_P(key), Z_ARRVAL_P(return_value));
	if (success) {
		ZEND_TRY_ASSIGN_REF_TRUE(success);
	}
}

PHP_FUNCTION(apcu_fetch)
{
	zval *key, *success = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ZVAL(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(success)
	ZEND_PARSE_PARAMETERS_END();

	fetch_impl(key, success, return_value);
}

#if PHP_VERSION_ID >= 80400
ZEND_FRAMELESS_FUNCTION(apcu_fetch, 1) { fetch_impl(arg1, NULL, return_value); }
#endif

static bool exists(zend_string *key) { return nocopy_info(key, NULL); }

static void exists_impl(zval *key, zval *return_value)
{
	if (Z_TYPE_P(key) != IS_ARRAY) {
		if (!nocopy_apc_on) {
			RETURN_FALSE;
		}
		if (!nocopy_tracked()) {
			RETURN_THROWS();
		}
		zend_string *ks = key_str(key);
		bool r = exists(ks);
		zend_string_release(ks);
		RETURN_BOOL(r);
	}
	array_init(return_value);
	if (!nocopy_apc_on) {
		return;
	}
	if (!nocopy_tracked()) {
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

PHP_FUNCTION(apcu_exists)
{
	zval *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(key)
	ZEND_PARSE_PARAMETERS_END();

	exists_impl(key, return_value);
}

#if PHP_VERSION_ID >= 80400
ZEND_FRAMELESS_FUNCTION(apcu_exists, 1) { exists_impl(arg1, return_value); }
#endif

static void delete_impl(zval *key, zval *return_value)
{
	if (Z_TYPE_P(key) == IS_OBJECT && instanceof_function(Z_OBJCE_P(key), iter_ce)) {
		nocopy_iter *it = ITER(Z_OBJ_P(key));
		if (!it->initialized) {
			zend_throw_error(NULL, "Trying to use uninitialized APCUIterator");
			RETURN_THROWS();
		}
		iter_snapshot(it);
		for (uint32_t i = 0; i < it->count; i++) {
			nocopy_drop(it->rows[i].key);
		}
		rows_free(it);
		it->built = false;
		RETURN_TRUE;
	}
	if (Z_TYPE_P(key) == IS_STRING) {
		RETURN_BOOL(nocopy_apc_on && nocopy_drop(Z_STR_P(key)));
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
		if (Z_TYPE_P(v) != IS_STRING || !nocopy_apc_on || !nocopy_drop(Z_STR_P(v))) {
			Z_TRY_ADDREF_P(v);
			add_next_index_zval(return_value, v);
		}
	} ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(apcu_delete)
{
	zval *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(key)
	ZEND_PARSE_PARAMETERS_END();

	delete_impl(key, return_value);
}

#if PHP_VERSION_ID >= 80400
ZEND_FRAMELESS_FUNCTION(apcu_delete, 1) { delete_impl(arg1, return_value); }
#endif

static void incdec_impl(zend_string *key, zend_long delta, zval *success, zend_long ttl, zval *return_value);

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

	incdec_impl(key, sign * step, success, ttl, return_value);
}

static void incdec_impl(zend_string *key, zend_long delta, zval *success, zend_long ttl, zval *return_value)
{
	if (success) {
		ZEND_TRY_ASSIGN_REF_FALSE(success);
	}
	if (!nocopy_apc_on) {
		RETURN_FALSE;
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	zend_long nv;
	if (!nocopy_add_long(key, delta, ttl, &nv)) {
		RETURN_FALSE;
	}
	if (success) {
		ZEND_TRY_ASSIGN_REF_TRUE(success);
	}
	RETURN_LONG(nv);
}

PHP_FUNCTION(apcu_inc) { incdec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }
PHP_FUNCTION(apcu_dec) { incdec(INTERNAL_FUNCTION_PARAM_PASSTHRU, -1); }

#if PHP_VERSION_ID >= 80400
#define FLF_INCDEC(name, sign) \
	ZEND_FRAMELESS_FUNCTION(name, 1) \
	{ \
		zval tmp; \
		zend_string *key; \
		Z_FLF_PARAM_STR(1, key, tmp); \
		incdec_impl(key, sign, NULL, 0, return_value); \
	flf_clean: \
		Z_FLF_PARAM_FREE_STR(1, tmp); \
	} \
	ZEND_FRAMELESS_FUNCTION(name, 2) \
	{ \
		zval tmp; \
		zend_string *key; \
		zend_long step; \
		Z_FLF_PARAM_STR(1, key, tmp); \
		Z_FLF_PARAM_LONG(2, step); \
		incdec_impl(key, sign * step, NULL, 0, return_value); \
	flf_clean: \
		Z_FLF_PARAM_FREE_STR(1, tmp); \
	}
FLF_INCDEC(apcu_inc, 1)
FLF_INCDEC(apcu_dec, -1)
#endif

PHP_FUNCTION(apcu_cas)
{
	zend_string *key;
	zend_long old, nw;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_LONG(old)
		Z_PARAM_LONG(nw)
	ZEND_PARSE_PARAMETERS_END();

	if (!nocopy_apc_on) {
		RETURN_FALSE;
	}
	RETURN_BOOL(nocopy_cas_long(key, old, nw));
}

PHP_FUNCTION(apcu_clear_cache)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (!nocopy_apc_on) {
		RETURN_TRUE;
	}
	nocopy_clear_all();
	RETURN_TRUE;
}

/* Single-flight per key, stored with add like APCu. */
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

	if (!nocopy_apc_on) {
		RETURN_NULL();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .add_only = true, .objects = NOCOPY_SERIALIZE_OBJECTS};
	nocopy_remember(key, &fci, &fcc, &o, 0, return_value);
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

	if (!nocopy_apc_on) {
		RETURN_NULL();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	nocopy_meta m;
	if (!nocopy_info(key, &m)) {
		RETURN_NULL();
	}
	array_init(return_value);
	add_assoc_long(return_value, "hits", 0);
	add_assoc_long(return_value, "access_time", m.created);
	add_assoc_long(return_value, "mtime", m.created);
	add_assoc_long(return_value, "creation_time", m.created);
	add_assoc_long(return_value, "deletion_time", 0);
	add_assoc_long(return_value, "ttl", m.expires ? m.expires - m.created : 0);
	add_assoc_long(return_value, "refs", 0);
}

typedef struct {
	zval *list; /* NULL: count only */
	zend_long entries;
} info_list;

static void info_add(void *arg, const zend_string *key, const nocopy_meta *m)
{
	info_list *l = arg;
	l->entries++;
	if (!l->list) {
		return;
	}
	zval row;
	array_init(&row);
	add_assoc_str(&row, "info", zend_string_init(ZSTR_VAL(key), ZSTR_LEN(key), 0));
	add_assoc_long(&row, "ttl", m->expires ? m->expires - m->created : 0);
	add_assoc_long(&row, "num_hits", 0);
	add_assoc_long(&row, "mtime", m->created);
	add_assoc_long(&row, "creation_time", m->created);
	add_assoc_long(&row, "deletion_time", 0);
	add_assoc_long(&row, "access_time", m->created);
	add_assoc_long(&row, "ref_count", 0);
	add_assoc_long(&row, "mem_size", m->size);
	add_next_index_zval(l->list, &row);
}

PHP_FUNCTION(apcu_cache_info)
{
	bool limited = false;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(limited)
	ZEND_PARSE_PARAMETERS_END();

	if (!nocopy_apc_on) {
		php_error_docref(NULL, E_WARNING, "No APC info available.  Perhaps APC is not enabled? Check apc.enabled in your ini file");
		RETURN_FALSE;
	}

	zval list;
	array_init(&list);
	info_list l = {.list = limited ? NULL : &list};
	nocopy_scan(info_add, &l);
	nocopy_stats_t st;
	nocopy_stats(&st);
	array_init(return_value);
	add_assoc_long(return_value, "num_slots", st.slots);
	add_assoc_long(return_value, "ttl", nocopy_ini_ttl);
	add_assoc_long(return_value, "num_hits", st.hits);
	add_assoc_long(return_value, "num_misses", st.misses);
	add_assoc_long(return_value, "num_inserts", st.inserts);
	add_assoc_long(return_value, "num_entries", l.entries);
	add_assoc_long(return_value, "expunges", st.expunges);
	add_assoc_long(return_value, "start_time", st.start_time);
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

	if (!nocopy_apc_on) {
		php_error_docref(NULL, E_WARNING, "No APC SMA info available.  Perhaps APC is disabled via apc.enabled?");
		RETURN_FALSE;
	}

	nocopy_stats_t st;
	nocopy_stats(&st);
	uint32_t nsh = st.shards;

	/* one APCu "segment" per shard: an entry only fits into free space of its own shard's slice */
	array_init(return_value);
	add_assoc_long(return_value, "num_seg", nsh);
	add_assoc_double(return_value, "seg_size", (double) (st.segments / nsh * st.segment_size));
	add_assoc_double(return_value, "avail_mem", (double) (st.segments * st.segment_size - st.used));
	if (limited) {
		return;
	}
	zval outer;
	array_init(&outer);
	for (uint32_t s = 0; s < nsh; s++) {
		size_t run_at[NOCOPY_MAX_SEGS], run_len[NOCOPY_MAX_SEGS];
		uint32_t runs = nocopy_free_runs(s, run_at, run_len, NOCOPY_MAX_SEGS);
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

void nocopy_apcu_minit(int module_number)
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
	iter_handlers.offset = offsetof(nocopy_iter, std);
	iter_handlers.free_obj = iter_free;
	iter_handlers.clone_obj = NULL;
}
