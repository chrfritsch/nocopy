/* nocopy: zero-copy shared-memory store with an apcu_* compatibility layer. This file: values in and out of the arena, nocopy_*, the module. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "nocopy_arena.h"
#include "ext/standard/info.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "SAPI.h"
#include "zend_interfaces.h"
#include "zend_system_id.h"
#include "zend_extensions.h"
#include "nocopy_arginfo.h"
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifndef RTLD_DEFAULT
# define RTLD_DEFAULT ((void *) 0)
#endif

#define NOCOPY_MAX_DEPTH 64
#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)

ZEND_DECLARE_MODULE_GLOBALS(nocopy)

static size_t arena_size = 32 * 1024 * 1024, local_size = 8 * 1024 * 1024;
static zend_long ini_hint = 4096;
zend_long nocopy_ini_ttl;
static char *shm_name; /* nocopy.shm_name: required, the arena every process attaches to */
static bool detour; /* Valkey on, or no arena: operations leave the local fast path (one check on it, set in MINIT) */
static char shm_attached[64]; /* the object mapped; empty: none (nocopy_a NULL: unavailable) */
static bool ini_enabled = true, ini_enable_cli, request_scope, apcu_compat;
bool nocopy_apc_on; /* apc.enabled, and apc.enable_cli under the CLI SAPI */
typedef int (*igb_ser_fn)(uint8_t **ret, size_t *ret_len, zval *z);
typedef int (*igb_unser_fn)(const uint8_t *buf, size_t buf_len, zval *z);
static bool use_igbinary; /* nocopy.serializer=igbinary, and igbinary resolved */
static igb_ser_fn igb_ser;
static igb_unser_fn igb_unser;
static zif_handler orig_handle_request; /* FrankenPHP's frankenphp_handle_request, wrapped */

/* The arena block: core header, the value, then the key string and the value's out-of-line parts. */
typedef struct {
	nocopy_entry h;
	zval val;
} php_entry;

#define ENTRY_SERIALIZED 1 /* val is a serialized string; unserialize on fetch */
#define ENTRY_IGBINARY 2   /* with ENTRY_SERIALIZED: written by igbinary */
/* nocopy_entry.user: nocopy_remember's compute time, ms (XFetch) */

#define KEY_OFF (SALIGN(sizeof(php_entry)) + _ZSTR_HEADER_SIZE) /* the key string comes first after the zval */
_Static_assert(KEY_OFF <= UINT8_MAX, "koff is 8 bits");

static inline zval *entry_val(nocopy_entry *e) { return &((php_entry *) e)->val; }
static inline zend_string *entry_key(const nocopy_entry *e) { return (zend_string *) (nocopy_key(e) - _ZSTR_HEADER_SIZE); }

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

/* ---- the key table: equal array keys stored once per value ---- */

/* Empties the table for the next value. */
static zend_always_inline void keys_reset(nocopy_keys *t)
{
	if (UNEXPECTED(++t->gen == 0)) {
		memset(t->slot, 0, NOCOPY_KEYS * sizeof(nocopy_kslot));
		t->gen = 1;
	}
}

static zend_always_inline nocopy_kslot *key_slot(nocopy_keys *t, zend_ulong h) { return &t->slot[h & (NOCOPY_KEYS - 1)]; }

/* An equal key was persisted earlier in this value. Depends only on the key sequence, so sizing and persisting agree. */
static zend_always_inline bool key_hit(const nocopy_keys *t, const nocopy_kslot *s, const zend_string *k, zend_ulong h)
{
	return s->gen == t->gen && s->h == h && zend_string_equals(s->s, k);
}

static zend_always_inline void key_put(const nocopy_keys *t, nocopy_kslot *s, const zend_string *k, zend_ulong h)
{
	s->s = k;
	s->h = h;
	s->gen = t->gen;
}

/* ---- sizing ---- */

typedef enum { SIZE_OK, SIZE_DEEP, SIZE_OBJECT, SIZE_RESOURCE } size_status;

typedef struct {
	size_t n;              /* out-of-line bytes persist_zval() will consume */
	nocopy_keys keys;       /* this value's view of the table (after keys_reset) */
	bool keep_shared_refs; /* a reference with refcount > 1 must stay shared: SIZE_OBJECT (serialize) */
} sizing;

static size_status size_zval(sizing *z, zval *v, int depth);

static size_status size_array(sizing *z, zend_array *a, int depth)
{
	if (zend_hash_num_elements(a) == 0) {
		return SIZE_OK;
	}
	z->n += SALIGN(sizeof(zend_array)) + SALIGN(HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : HT_USED_SIZE(a));
	size_status st = SIZE_OK;
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if ((st = size_zval(z, v, depth + 1)) != SIZE_OK) {
				return st;
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key) {
				nocopy_kslot *s = key_slot(&z->keys, b->h);
				if (!key_hit(&z->keys, s, b->key, b->h)) {
					z->n += SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(b->key)));
					key_put(&z->keys, s, b->key, b->h);
				}
			}
			if ((st = size_zval(z, &b->val, depth + 1)) != SIZE_OK) {
				return st;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return SIZE_OK;
}

static size_status size_zval(sizing *z, zval *v, int depth)
{
	if (depth > NOCOPY_MAX_DEPTH) {
		return SIZE_DEEP;
	}
	switch (Z_TYPE_P(v)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE:
			return SIZE_OK;
		case IS_REFERENCE:
			if (z->keep_shared_refs && Z_REFCOUNT_P(v) > 1) {
				return SIZE_OBJECT;
			}
			return size_zval(z, Z_REFVAL_P(v), depth + 1);
		case IS_STRING:
			z->n += SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN_P(v)));
			return SIZE_OK;
		case IS_ARRAY:
			return size_array(z, Z_ARR_P(v), depth);
		case IS_RESOURCE:
			return SIZE_RESOURCE;
		default:
			return SIZE_OBJECT;
	}
}

/* ---- persisting: values into an arena block, opcache-style immutable ---- */

typedef struct {
	char *cursor; /* next free byte of the block */
	nocopy_keys keys;
} persist;

/* The empty array every entry points to: in the arena, so it has the same address in every process that maps it (unlike zend_empty_array). */
#define ARENA_EMPTY ((zend_array *) nocopy_a->user)

/* Idempotent: every process writes the same bytes. */
static void arena_empty_init(void)
{
	zend_array *a = ARENA_EMPTY;
	uint32_t *hash = (uint32_t *) (a + 1);
	ZEND_ASSERT(sizeof(zend_array) + 2 * sizeof(uint32_t) <= sizeof(nocopy_a->user));
	memcpy(a, &zend_empty_array, sizeof(zend_array));
	a->pDestructor = NULL;
	hash[0] = hash[1] = HT_INVALID_IDX;
	HT_SET_DATA_ADDR(a, hash);
}

static void *take(persist *p, size_t n)
{
	void *r = p->cursor;
	p->cursor += SALIGN(n);
	return r;
}

static zend_string *persist_str(persist *p, const zend_string *s)
{
	size_t size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(s));
	zend_string *r = take(p, size);
	memcpy(r, s, size); /* keys arrive hashed; values stay unhashed until used as a key (benign idempotent write) */
	GC_SET_REFCOUNT(r, 2);
	/* not IS_STR_PERMANENT: that promises process lifetime (PCRE then keeps the pointer as a cache key) */
	GC_TYPE_INFO(r) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	return r;
}

static void persist_zval(persist *p, zval *z);

static zend_array *persist_array(persist *p, const zend_array *src)
{
	if (zend_hash_num_elements(src) == 0) {
		return ARENA_EMPTY;
	}
	zend_array *ht = take(p, sizeof(zend_array));
	memcpy(ht, src, sizeof(zend_array));
	ht->pDestructor = NULL; /* a function address: differs between processes; immutable arrays are never destroyed */

	size_t size = HT_IS_PACKED(src) ? HT_PACKED_USED_SIZE(src) : HT_USED_SIZE(src);
	void *data = take(p, size);
	memcpy(data, HT_GET_DATA_ADDR(src), size);
	HT_SET_DATA_ADDR(ht, data);

	ht->nInternalPointer = 0;
	GC_SET_REFCOUNT(ht, 2);
	GC_TYPE_INFO(ht) = GC_ARRAY | IS_ARRAY_IMMUTABLE;

	if (HT_IS_PACKED(ht)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(ht, v) {
			persist_zval(p, v);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(ht, b) {
			if (b->key) {
				nocopy_kslot *s = key_slot(&p->keys, b->h);
				if (key_hit(&p->keys, s, b->key, b->h)) {
					b->key = (zend_string *) s->s;
				} else {
					b->key = persist_str(p, b->key);
					key_put(&p->keys, s, b->key, b->h);
				}
			}
			persist_zval(p, &b->val);
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

/* Rewrites *z in place so everything it points to lives in the block. */
static void persist_zval(persist *p, zval *z)
{
	switch (Z_TYPE_P(z)) {
		case IS_REFERENCE:
			ZVAL_COPY_VALUE(z, Z_REFVAL_P(z));
			persist_zval(p, z);
			break;
		case IS_STRING:
			Z_STR_P(z) = persist_str(p, Z_STR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		case IS_ARRAY:
			Z_ARR_P(z) = persist_array(p, Z_ARR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		default: /* scalars: sizing already refused everything else */
			break;
	}
}

/* Fills e after its header: the key string (at KEY_OFF), then the value. */
static void persist_entry(nocopy_entry *e, const zend_string *key, zval *val, nocopy_keys *keys)
{
	if (Z_TYPE_P(val) == IS_ARRAY) {
		keys_reset(keys);
	}
	persist p = {.cursor = (char *) e + SALIGN(sizeof(php_entry)), .keys = *keys};
	zend_string *k = persist_str(&p, key);
	ZEND_ASSERT(ZSTR_VAL(k) == (char *) e + e->koff);
	(void) k;
	ZVAL_COPY_VALUE(entry_val(e), val);
	persist_zval(&p, entry_val(e));
	ZEND_ASSERT(p.cursor == (char *) e + e->size);
}

/* ---- moving: merge byte-copied a block, its pointers still aim into the old one ---- */

typedef struct {
	uintptr_t from;
	size_t size;
	ptrdiff_t by;
} rebase;

static zend_always_inline void *moved(const rebase *r, void *p)
{
	return (uintptr_t) p - r->from < r->size ? (char *) p + r->by : p; /* ARENA_EMPTY stays */
}

static void rebase_zval(const rebase *r, zval *z)
{
	switch (Z_TYPE_P(z)) {
		case IS_STRING:
			Z_STR_P(z) = moved(r, Z_STR_P(z));
			ZSTR_H(Z_STR_P(z)) = 0; /* readers may have been hashing the old copy while it was copied: rehash on use */
			break;
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(z) = moved(r, Z_ARR_P(z));
			if (a == ARENA_EMPTY) {
				break;
			}
			a->arData = moved(r, a->arData);
			if (HT_IS_PACKED(a)) {
				zval *v;
				ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
					rebase_zval(r, v);
				} ZEND_HASH_FOREACH_END();
			} else {
				Bucket *b;
				ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
					if (b->key) {
						b->key = moved(r, b->key);
					}
					rebase_zval(r, &b->val);
				} ZEND_HASH_FOREACH_END();
			}
			break;
		}
	}
}

void nocopy_rebase(nocopy_entry *e, const nocopy_entry *from)
{
	rebase r = {.from = (uintptr_t) from, .size = from->size, .by = (char *) e - (const char *) from};
	rebase_zval(&r, entry_val(e));
}

/* ---- leaving shared memory: deep copies ---- */

static bool in_arena(const void *p)
{
	return (uintptr_t) p - (uintptr_t) nocopy_a < nocopy_a->size;
}

static void unshare_zval(nocopy_keys *keys, zval *z);
static void unshare_contents(nocopy_keys *keys, zend_array *a);

/* Deep copy of an arena value into ordinary request memory. */
static void copy_out(nocopy_keys *keys, zval *dst, const zval *src)
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
			unshare_contents(keys, Z_ARR_P(dst));
			break;
		}
		default:
			ZVAL_COPY_VALUE(dst, src);
	}
}

/* Repoints every key and value of a (private) array that still lives in the arena. Stored values are at most NOCOPY_MAX_DEPTH deep. */
static void unshare_contents(nocopy_keys *keys, zend_array *a)
{
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			unshare_zval(keys, v);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key && in_arena(b->key)) {
				nocopy_kslot *s = key_slot(keys, b->h);
				if (s->gen == keys->gen && s->s == b->key) { /* a deduped key: share its copy */
					b->key = zend_string_copy(s->copy);
				} else {
					key_put(keys, s, b->key, b->h);
					b->key = s->copy = zend_string_init(ZSTR_VAL(b->key), ZSTR_LEN(b->key), 0);
					ZSTR_H(b->key) = b->h;
				}
				HT_FLAGS(a) &= ~HASH_FLAG_STATIC_KEYS;
			}
			unshare_zval(keys, &b->val);
		} ZEND_HASH_FOREACH_END();
	}
}

/* Replaces an arena string or array inside a copied-out array with a private copy. */
static void unshare_zval(nocopy_keys *keys, zval *z)
{
	if ((Z_TYPE_P(z) == IS_STRING || Z_TYPE_P(z) == IS_ARRAY) && in_arena(Z_COUNTED_P(z))) {
		zval t;
		copy_out(keys, &t, z);
		ZVAL_COPY_VALUE(z, &t);
	}
}

/* ---- copy mode: private copies kept for the request ---- */

/* The slot for e's copy, or NULL: nothing worth keeping, or not in a request. */
static zend_always_inline nocopy_local *local_slot(const nocopy_entry *e, const zval *v)
{
	/* short strings copy faster than a cold slot line loads (100k distinct 100 B strings: 37 -> 45 ns kept) */
	if ((uint32_t) (Z_TYPE_P(v) - IS_STRING) > 1 || !NOCOPY_G(local_ok) || e->size > local_size
	 || (Z_TYPE_P(v) == IS_STRING ? e->size < 512 : !zend_hash_num_elements(Z_ARR_P(v)))) {
		return NULL;
	}
	if (UNEXPECTED(!NOCOPY_G(local))) {
		NOCOPY_G(local) = ecalloc(NOCOPY_LOCAL, sizeof(nocopy_local));
	}
	return &NOCOPY_G(local)[(((uintptr_t) e >> 3) * 0x9E3779B97F4A7C15ULL) >> 52];
}

static void local_flush(void)
{
	for (int i = 0; i < NOCOPY_LOCAL; i++) {
		if (NOCOPY_G(local)[i].e) {
			zval_ptr_dtor(&NOCOPY_G(local)[i].v);
			NOCOPY_G(local)[i].e = NULL;
		}
	}
	NOCOPY_G(local_bytes) = 0;
}

/* ponytail: over budget flushes everything; per-slot eviction if working sets near the budget thrash */
static void local_put(nocopy_local *l, const nocopy_entry *e, const zval *copy)
{
	if (l->e) {
		NOCOPY_G(local_bytes) -= l->size;
		zval_ptr_dtor(&l->v);
		l->e = NULL; /* or the flush below releases it again */
	}
	if (NOCOPY_G(local_bytes) + e->size > local_size) {
		local_flush();
	}
	l->e = e;
	l->seq = e->seq;
	l->size = e->size;
	ZVAL_COPY(&l->v, copy);
	NOCOPY_G(local_bytes) += e->size;
}

/* ---- writes ---- */

static bool serialize_value(zval *val, zval *out, bool *igb)
{
	*igb = use_igbinary;
	if (use_igbinary) {
		uint8_t *b;
		size_t len;
		if (igb_ser(&b, &len, val) != 0 || EG(exception)) {
			return false;
		}
		ZVAL_STRINGL(out, (char *) b, len);
		efree(b);
		return true;
	}
	smart_str buf = {0};
	BG(serialize_lock)++;
	php_serialize_data_t h = php_var_serialize_init();
	php_var_serialize(&buf, val, &h);
	php_var_serialize_destroy(h);
	BG(serialize_lock)--;
	if (EG(exception)) {
		smart_str_free(&buf);
		return false;
	}
	smart_str_0(&buf);
	ZVAL_STR(out, buf.s);
	return true;
}

/* Sizes the value, allocates and fills its entry. NOCOPY_OK: p holds it, ready for publish().
 * p NULL (no arena): only the checks, so a value a store would refuse is refused alike; then NOCOPY_NO_ROOM. */
static nocopy_status build(nocopy_put *p, zend_string *key, zval *val, const nocopy_opts *o)
{
	nocopy_keys *keys = &NOCOPY_G(keys);
	if (Z_TYPE_P(val) == IS_ARRAY) {
		keys_reset(keys);
	}
	sizing z = {.keys = *keys, .keep_shared_refs = o->objects == NOCOPY_SERIALIZE_OBJECTS};
	zval ser;
	bool serialized = false, igb = false;
	size_status st = size_zval(&z, val, 0);
	if (st != SIZE_OK) {
		if (o->objects == NOCOPY_REFUSE_OBJECTS) {
			if (st == SIZE_DEEP) {
				zend_value_error("nocopy: value is nested too deeply");
			} else {
				zend_argument_type_error(2, "must not contain objects or resources");
			}
			return NOCOPY_REFUSED;
		}
		if (st == SIZE_RESOURCE) {
			php_error_docref(NULL, E_WARNING, "Cannot store resources in apcu cache");
			return NOCOPY_REFUSED;
		}
		if (!serialize_value(val, &ser, &igb)) {
			return NOCOPY_REFUSED;
		}
		val = &ser;
		serialized = true;
		z.n = SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN(ser)));
	}

	size_t size = SALIGN(sizeof(php_entry)) + SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(key))) + z.n;
	nocopy_entry *e = !p ? NULL : nocopy_put_begin(p, &NOCOPY_G(ctx), zend_string_hash_val(key), (uint32_t) ZSTR_LEN(key), KEY_OFF, size,
		o->ttl ? o->ttl : nocopy_ini_ttl);
	if (e) {
		e->flags = serialized ? ENTRY_SERIALIZED | (igb ? ENTRY_IGBINARY : 0) : 0;
		persist_entry(e, key, val, keys);
	}
	if (serialized) {
		zend_string_release(Z_STR(ser));
	}
	return e ? NOCOPY_OK : NOCOPY_NO_ROOM;
}

static nocopy_status publish(nocopy_put *p, bool add_only)
{
	switch (nocopy_put_commit(p, add_only)) {
		case PUT_OK: return NOCOPY_OK;
		case PUT_EXISTS: return NOCOPY_EXISTS;
		default: return NOCOPY_NO_ROOM;
	}
}

static nocopy_status vk_store(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user);

nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o)
{
	if (UNEXPECTED(detour)) {
		return nocopy_a ? vk_store(key, val, o, 0) : build(NULL, key, val, o);
	}
	nocopy_put p;
	nocopy_status s = build(&p, key, val, o);
	return s == NOCOPY_OK ? publish(&p, o->add_only) : s;
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

/* After a change in Valkey: a sibling's fill that read the old value sees the guard move and drops its copy. */
static void vk_forget(zend_string *key)
{
	__atomic_add_fetch(&nocopy_a->inval_gen, 1, __ATOMIC_SEQ_CST);
	nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
}

bool nocopy_drop(zend_string *key)
{
	if (UNEXPECTED(detour)) {
		if (!nocopy_a) {
			return false;
		}
		int r = nocopy_vk_ready() ? nocopy_vk_unlink(ZSTR_VAL(key), ZSTR_LEN(key)) : -1;
		vk_forget(key);
		return r == 1;
	}
	return drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
}

void nocopy_clear_all(void)
{
	if (UNEXPECTED(!nocopy_a)) {
		return;
	}
	if (nocopy_vk_on && nocopy_vk_ready()) {
		nocopy_vk_clear();
	}
	nocopy_clear();
}

bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out)
{
	if (UNEXPECTED(detour)) {
		int64_t v = 0;
		if (!nocopy_a) {
			*out = 0;
			return false;
		}
		zend_long t = ttl ? ttl : nocopy_ini_ttl;
		int r = nocopy_vk_ready() ? nocopy_vk_incr(ZSTR_VAL(key), ZSTR_LEN(key), delta, (int64_t) t * 1000, &v) : -1;
		vk_forget(key); /* the next read fetches it */
		*out = (zend_long) v;
		return r == 1;
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
		const nocopy_opts create = {.ttl = ttl, .add_only = true, .objects = NOCOPY_REFUSE_OBJECTS};
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
	if (UNEXPECTED(detour)) {
		if (!nocopy_a) {
			return false;
		}
		int r = nocopy_vk_ready() ? nocopy_vk_cas(ZSTR_VAL(key), ZSTR_LEN(key), old, nw) : -1;
		vk_forget(key);
		return r == 1;
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

/* Unserialize with a fresh state: php_unserialize_with_options() would share the outer
 * unserialize()'s reference table when called re-entrantly (e.g. from Serializable::unserialize). */
static bool cache_unserialize(zval *rv, const char *buf, size_t len, uint8_t flags)
{
	bool ok;
	if (flags & ENTRY_IGBINARY) {
		ZVAL_NULL(rv);
		ok = igb_unser && igb_unser((const uint8_t *) buf, len, rv) == 0; /* no igb_unser: a miss */
	} else {
		const unsigned char *p = (const unsigned char *) buf;
		BG(serialize_lock)++; /* forces a private state instead of the outer call's */
		php_unserialize_data_t h = php_var_unserialize_init();
		ok = php_var_unserialize(rv, &p, p + len, &h);
		php_var_unserialize_destroy(h);
		BG(serialize_lock)--;
	}
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
static zend_always_inline bool local_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	nocopy_ctx *c = &NOCOPY_G(ctx);
	bool copy = !NOCOPY_G(zero_copy);
	nocopy_borrow_begin(c);
	nocopy_entry *e = nocopy_find(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	bool hit = e != NULL;
	ZVAL_NULL(rv);
	if (e) {
		zval *v = entry_val(e);
		if (exp) {
			*exp = e->expires;
			*delta = e->user;
		}
		if (e->flags & ENTRY_SERIALIZED) {
			hit = cache_unserialize(rv, Z_STRVAL_P(v), Z_STRLEN_P(v), e->flags);
		} else if (copy) {
			nocopy_local *l = local_slot(e, v);
			if (l && l->e == e && l->seq == e->seq) {
				ZVAL_COPY(rv, &l->v); /* same version as last time: share it, COW keeps it intact */
			} else {
				nocopy_keys *keys = &NOCOPY_G(keys);
				if (Z_TYPE_P(v) == IS_ARRAY) {
					keys_reset(keys);
				}
				copy_out(keys, rv, v);
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

/* ---- Valkey tier: the cache of record behind the local arena ---- */

#define VK_HDR 14 /* 0x00, flags (ENTRY_IGBINARY), int64 expires, uint32 compute ms; then the serialized value. Integers: decimal, for INCRBY */

static zend_string *vk_encode(zval *val, time_t expires, uint32_t user)
{
	if (Z_TYPE_P(val) == IS_LONG) {
		return zend_long_to_str(Z_LVAL_P(val));
	}
	zval ser;
	bool igb;
	if (!serialize_value(val, &ser, &igb)) {
		return NULL;
	}
	zend_string *s = zend_string_alloc(VK_HDR + Z_STRLEN(ser), 0);
	char *b = ZSTR_VAL(s);
	int64_t x = expires;
	b[0] = 0;
	b[1] = igb ? ENTRY_IGBINARY : 0;
	memcpy(b + 2, &x, 8);
	memcpy(b + 10, &user, 4);
	memcpy(b + VK_HDR, Z_STRVAL(ser), Z_STRLEN(ser) + 1);
	zval_ptr_dtor(&ser);
	return s;
}

/* raw: a private copy, so unserialize may run user code that calls Valkey again */
static bool vk_decode(const zend_string *raw, zval *rv, time_t *expires, uint32_t *user)
{
	const char *b = ZSTR_VAL(raw);
	size_t n = ZSTR_LEN(raw);
	*expires = 0;
	*user = 0;
	if (n && b[0] != 0) {
		zend_long l;
		if (is_numeric_string(b, n, &l, NULL, false) != IS_LONG) {
			return false; /* not written by nocopy */
		}
		ZVAL_LONG(rv, l);
		return true;
	}
	if (n < VK_HDR) {
		return false;
	}
	int64_t x;
	memcpy(&x, b + 2, 8);
	memcpy(user, b + 10, 4);
	*expires = (time_t) x;
	if (x && x < time(NULL)) {
		return false;
	}
	return cache_unserialize(rv, b + VK_HDR, n - VK_HDR, ENTRY_SERIALIZED | (uint8_t) b[1]);
}

/* Decodes a value read from Valkey into rv and keeps a local copy. Add-only: a local write since our miss is newer. */
static bool vk_fill(zend_string *key, const zend_string *raw, zval *rv, time_t *exp, uint32_t *delta)
{
	time_t expires;
	uint32_t user;
	if (!vk_decode(raw, rv, &expires, &user)) {
		return false;
	}
	const nocopy_opts o = {.ttl = expires ? MAX(1, expires - time(NULL)) : 0, .objects = NOCOPY_SERIALIZE_OBJECTS};
	nocopy_put p;
	if (build(&p, key, rv, &o) == NOCOPY_OK) {
		p.e->user = user;
		publish(&p, true);
	}
	if (exp) {
		*exp = expires;
		*delta = user;
	}
	return true;
}

/* Local miss: GET, then keep a local copy, unless an invalidation was applied meanwhile (it may have been for this key). */
static zend_never_inline bool vk_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (!nocopy_a || !nocopy_vk_ready()) { /* no arena: nothing to serve, with or without Valkey */
		ZVAL_NULL(rv);
		return false; /* no invalidations: local data may be stale */
	}
	if (local_fetch(key, rv, exp, delta)) {
		return true;
	}
	uint64_t gen = __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST);
	zend_string *raw = NULL;
	if (nocopy_vk_mget(1, &key, &raw) != 1 || !raw) {
		return false;
	}
	bool ok = vk_fill(key, raw, rv, exp, delta);
	zend_string_release(raw);
	if (ok && __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST) != gen) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
	}
	return ok;
}

static zend_always_inline bool fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (UNEXPECTED(detour)) {
		return vk_fetch(key, rv, exp, delta);
	}
	return local_fetch(key, rv, exp, delta);
}

/* Valkey first for add (NX decides), else local first: a sibling's invalidation can then only remove our copy, never outlive it. */
static nocopy_status vk_store(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user)
{
	if (!nocopy_vk_ready()) {
		return NOCOPY_NO_ROOM;
	}
	nocopy_put p;
	nocopy_status s = NOCOPY_OK;
	if (!o->add_only && (s = build(&p, key, val, o)) == NOCOPY_OK) {
		p.e->user = user;
		publish(&p, false);
	}
	if (s == NOCOPY_REFUSED) {
		return s;
	}
	zend_long ttl = o->ttl ? o->ttl : nocopy_ini_ttl;
	time_t expires = ttl ? time(NULL) + ttl : 0;
	zend_string *enc = vk_encode(val, expires, user);
	if (!enc) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
		return NOCOPY_REFUSED;
	}
	int r = nocopy_vk_set(ZSTR_VAL(key), ZSTR_LEN(key), ZSTR_VAL(enc), ZSTR_LEN(enc), expires ? ((int64_t) expires + 1) * 1000 : 0, o->add_only);
	zend_string_release(enc);
	if (r != 1) {
		nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
		return r == 0 ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
	}
	if (o->add_only && (s = build(&p, key, val, o)) == NOCOPY_OK) {
		p.e->user = user;
		publish(&p, false);
	} else if (s == NOCOPY_REFUSED) {
		nocopy_vk_unlink(ZSTR_VAL(key), ZSTR_LEN(key)); /* a resource: APCu refuses it */
		return s;
	}
	return NOCOPY_OK;
}

bool nocopy_fetch(zend_string *key, zval *rv) { return fetch(key, rv, NULL, NULL); }

void nocopy_fetch_multi(HashTable *keys, HashTable *out)
{
	zval *k, t;
	if (!detour) {
		ZEND_HASH_FOREACH_VAL(keys, k) {
			ZVAL_DEREF(k);
			if (Z_TYPE_P(k) == IS_STRING && local_fetch(Z_STR_P(k), &t, NULL, NULL)) {
				zend_symtable_update(out, Z_STR_P(k), &t);
			}
		} ZEND_HASH_FOREACH_END();
		return;
	}
	if (!nocopy_a || !nocopy_vk_ready()) {
		return;
	}
	zend_string **miss = safe_emalloc(zend_hash_num_elements(keys), sizeof *miss, 0);
	uint32_t n = 0;
	ZEND_HASH_FOREACH_VAL(keys, k) {
		ZVAL_DEREF(k);
		if (Z_TYPE_P(k) != IS_STRING) {
			continue;
		}
		if (local_fetch(Z_STR_P(k), &t, NULL, NULL)) {
			zend_symtable_update(out, Z_STR_P(k), &t);
		} else {
			miss[n++] = Z_STR_P(k); /* borrowed: keys outlives this call */
			ZVAL_NULL(&t);
			zend_symtable_update(out, Z_STR_P(k), &t); /* keeps the requested order; removed if not found */
		}
	} ZEND_HASH_FOREACH_END();
	if (n) {
		zend_string **raw = ecalloc(n, sizeof *raw);
		uint64_t gen = __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST);
		bool got = nocopy_vk_mget(n, miss, raw) == 1;
		for (uint32_t i = 0; i < n; i++) {
			if (raw[i] && !EG(exception) && vk_fill(miss[i], raw[i], &t, NULL, NULL)) {
				zend_symtable_update(out, miss[i], &t);
			} else {
				zend_symtable_del(out, miss[i]);
			}
			if (raw[i]) {
				zend_string_release(raw[i]);
			}
		}
		if (got && __atomic_load_n(&nocopy_a->inval_gen, __ATOMIC_SEQ_CST) != gen) {
			for (uint32_t i = 0; i < n; i++) {
				nocopy_drop_local(ZSTR_VAL(miss[i]), ZSTR_LEN(miss[i]));
			}
		}
		efree(raw);
	}
	efree(miss);
}

static void mark_failed(HashTable *failed, zend_string *key)
{
	if (failed && !EG(exception)) {
		zval m;
		ZVAL_LONG(&m, -1);
		zend_symtable_update(failed, key, &m);
	}
}

/* vk_store for many keys: local copies first, then every SET in one pipeline. */
static bool vk_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed)
{
	zend_ulong h;
	zend_string *k;
	zval *v;
	bool all = true;
	if (!nocopy_vk_ready()) {
		ZEND_HASH_FOREACH_KEY(vals, h, k) {
			zend_string *key = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
			mark_failed(failed, key);
			zend_string_release(key);
		} ZEND_HASH_FOREACH_END();
		return false;
	}
	uint32_t cap = zend_hash_num_elements(vals), m = 0;
	zend_string **keys = safe_emalloc(cap, sizeof *keys, 0), **enc = safe_emalloc(cap, sizeof *enc, 0);
	nocopy_vk_kv *kv = safe_emalloc(cap, sizeof *kv, 0);
	zend_long ttl = o->ttl ? o->ttl : nocopy_ini_ttl;
	time_t expires = ttl ? time(NULL) + ttl : 0;
	ZEND_HASH_FOREACH_KEY_VAL(vals, h, k, v) {
		zend_string *key = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		nocopy_put p;
		nocopy_status s = build(&p, key, v, o);
		if (s == NOCOPY_OK) {
			p.e->user = 0;
			publish(&p, false);
		}
		zend_string *e = s == NOCOPY_REFUSED ? NULL : vk_encode(v, expires, 0);
		if (!e) {
			nocopy_drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
			all = false;
			mark_failed(failed, key);
			zend_string_release(key);
			if (EG(exception)) {
				break;
			}
			continue;
		}
		keys[m] = key;
		enc[m] = e;
		kv[m] = (nocopy_vk_kv) {.k = ZSTR_VAL(key), .kl = ZSTR_LEN(key), .v = ZSTR_VAL(e), .vl = ZSTR_LEN(e)};
		m++;
	} ZEND_HASH_FOREACH_END();
	int8_t *res = emalloc(m + 1);
	if (m) {
		nocopy_vk_mset(m, kv, expires ? ((int64_t) expires + 1) * 1000 : 0, res);
	}
	for (uint32_t i = 0; i < m; i++) {
		if (res[i] != 1) {
			nocopy_drop_local(ZSTR_VAL(keys[i]), ZSTR_LEN(keys[i]));
			all = false;
			mark_failed(failed, keys[i]);
		}
		zend_string_release(keys[i]);
		zend_string_release(enc[i]);
	}
	efree(res), efree(kv), efree(enc), efree(keys);
	return all;
}

bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed)
{
	if (nocopy_vk_on && !o->add_only && nocopy_a) {
		return vk_store_multi(vals, o, failed); /* ponytail: add stays one round trip per key (NX decides first) */
	}
	zend_ulong h;
	zend_string *k;
	zval *v;
	bool all = true;
	ZEND_HASH_FOREACH_KEY_VAL(vals, h, k, v) {
		zend_string *key = k ? zend_string_copy(k) : zend_long_to_str((zend_long) h);
		if (nocopy_store(key, v, o) != NOCOPY_OK) {
			all = false;
			mark_failed(failed, key);
		}
		zend_string_release(key);
		if (EG(exception)) {
			break;
		}
	} ZEND_HASH_FOREACH_END();
	return all;
}

static nocopy_meta meta_of(const nocopy_entry *e)
{
	return (nocopy_meta) {.created = e->created, .expires = e->expires, .size = e->size, .seq = e->seq};
}

static bool local_info(zend_string *key, nocopy_meta *m)
{
	nocopy_ctx *c = &NOCOPY_G(ctx);
	nocopy_borrow_begin(c);
	nocopy_entry *e = nocopy_find(c, zend_string_hash_val(key), ZSTR_VAL(key), (uint32_t) ZSTR_LEN(key));
	if (e && m) {
		*m = meta_of(e);
	}
	nocopy_borrow_end(c);
	return e != NULL;
}

bool nocopy_info(zend_string *key, nocopy_meta *m)
{
	if (!detour) {
		return local_info(key, m);
	}
	if (!nocopy_a || !nocopy_vk_ready()) {
		return false;
	}
	if (!local_info(key, m)) {
		zval t;
		if (!fetch(key, &t, NULL, NULL)) {
			return false;
		}
		zval_ptr_dtor(&t);
		if (!local_info(key, m) && m) {
			*m = (nocopy_meta) {.created = time(NULL)}; /* in Valkey, not kept here */
		}
	}
	return true;
}

void nocopy_scan(nocopy_scan_fn fn, void *arg)
{
	if (!nocopy_a || (nocopy_vk_on && !nocopy_vk_ready())) {
		return;
	}
	nocopy_ctx *c = &NOCOPY_G(ctx);
	nocopy_borrow_begin(c); /* pins, not the lock: fn may allocate */
	nocopy_entry *e;
	for (uint32_t sh = 0, i = 0; (e = nocopy_scan_next(c, &sh, &i));) {
		nocopy_meta m = meta_of(e);
		fn(arg, entry_key(e), &m);
	}
	nocopy_borrow_end(c);
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
	int r = nocopy_lease_take(h, NOCOPY_G(ctx).slot, time(NULL), LEASE_TTL);
	if (r == NOCOPY_LEASE_TAKEN) {
		NOCOPY_G(leases)[NOCOPY_G(nleases)++] = h;
	}
	return r;
}

static void lease_drop(void)
{
	nocopy_lease_drop(NOCOPY_G(leases)[--NOCOPY_G(nleases)], NOCOPY_G(ctx).slot);
}

/* Runs the callback, measures it, stores the result with its compute time. */
static void compute(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, const nocopy_opts *o, zval *rv)
{
	zval arg;
	ZVAL_STR(&arg, key);
	fci->param_count = 1;
	fci->params = &arg;
	fci->retval = rv;
	double t0 = now_s(CLOCK_MONOTONIC);
	if (zend_call_function(fci, fcc) == FAILURE || EG(exception)) {
		return;
	}
	double ms = (now_s(CLOCK_MONOTONIC) - t0) * 1000;
	uint32_t user = ms >= UINT32_MAX ? UINT32_MAX : (uint32_t) ms;
	if (UNEXPECTED(!nocopy_a)) {
		build(NULL, key, rv, o);
		return;
	}
	if (nocopy_vk_on) {
		vk_store(key, rv, o, user);
		return;
	}
	nocopy_put p;
	if (build(&p, key, rv, o) == NOCOPY_OK) {
		p.e->user = user;
		publish(&p, o->add_only);
	}
}

void nocopy_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, const nocopy_opts *o, double beta, zval *rv)
{
	time_t exp;
	uint32_t delta;
	if (UNEXPECTED(detour) && !nocopy_a) {
		compute(key, fci, fcc, o, rv); /* no leases without an arena */
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
		compute(key, fci, fcc, o, rv);
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
	compute(key, fci, fcc, o, rv);
	if (mine) {
		lease_drop();
	}
}

/* ---- nocopy_* ---- */

static void put_impl(zend_string *key, zval *val, zend_long ttl, bool serialize, zval *return_value)
{
	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .objects = serialize ? NOCOPY_SERIALIZE_OBJECTS : NOCOPY_REFUSE_OBJECTS};
	nocopy_status st = nocopy_store(key, val, &o);
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(st == NOCOPY_OK);
}

PHP_FUNCTION(nocopy_put)
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

	put_impl(key, val, ttl, serialize, return_value);
}

PHP_FUNCTION(nocopy_get)
{
	zend_string *key;
	zval *hit = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(hit)
	ZEND_PARSE_PARAMETERS_END();

	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	bool found = nocopy_fetch(key, return_value);
	if (hit) {
		ZEND_TRY_ASSIGN_REF_BOOL(hit, found);
	}
}

#if PHP_VERSION_ID >= 80400
/* Frameless variants (PHP 8.4+): a call the compiler resolved skips the VM's call frame. */
ZEND_FRAMELESS_FUNCTION(nocopy_get, 1)
{
	zval tmp;
	zend_string *key;
	Z_FLF_PARAM_STR(1, key, tmp);
	if (nocopy_tracked()) {
		nocopy_fetch(key, return_value);
	}
flf_clean:
	Z_FLF_PARAM_FREE_STR(1, tmp);
}

ZEND_FRAMELESS_FUNCTION(nocopy_put, 2)
{
	zval tmp;
	zend_string *key;
	Z_FLF_PARAM_STR(1, key, tmp);
	put_impl(key, arg2, 0, false, return_value);
flf_clean:
	Z_FLF_PARAM_FREE_STR(1, tmp);
}

ZEND_FRAMELESS_FUNCTION(nocopy_put, 3)
{
	zval tmp;
	zend_string *key;
	zend_long ttl;
	Z_FLF_PARAM_STR(1, key, tmp);
	Z_FLF_PARAM_LONG(3, ttl);
	put_impl(key, arg2, ttl, false, return_value);
flf_clean:
	Z_FLF_PARAM_FREE_STR(1, tmp);
}
#endif

PHP_FUNCTION(nocopy_delete)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(nocopy_drop(key));
}

PHP_FUNCTION(nocopy_get_multiple)
{
	HashTable *keys;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY_HT(keys)
	ZEND_PARSE_PARAMETERS_END();

	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	array_init(return_value);
	nocopy_fetch_multi(keys, Z_ARRVAL_P(return_value));
}

PHP_FUNCTION(nocopy_put_multiple)
{
	HashTable *vals;
	zend_long ttl = 0;
	bool serialize = false;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ARRAY_HT(vals)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
		Z_PARAM_BOOL(serialize)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(2, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .objects = serialize ? NOCOPY_SERIALIZE_OBJECTS : NOCOPY_REFUSE_OBJECTS};
	bool ok = nocopy_store_multi(vals, &o, NULL);
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(ok);
}

PHP_FUNCTION(nocopy_remember)
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
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .objects = NOCOPY_SERIALIZE_OBJECTS};
	nocopy_remember(key, &fci, &fcc, &o, beta, return_value);
	if (EG(exception)) {
		RETURN_THROWS();
	}
}

PHP_FUNCTION(nocopy_shared_with)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_STRING(!nocopy_a ? "none" : nocopy_vk_on ? "valkey" : "host");
}

PHP_FUNCTION(nocopy_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();
	nocopy_stats_t s = {0};
	if (nocopy_a) {
		nocopy_stats(&s);
	}
	array_init(return_value);
	add_assoc_long(return_value, "size", nocopy_a ? (zend_long) nocopy_a->size : 0);
	add_assoc_long(return_value, "used", s.used);
	add_assoc_long(return_value, "segment_size", s.segment_size);
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
	if (shm_attached[0]) {
		add_assoc_string(return_value, "shm_name", shm_attached);
	} else {
		add_assoc_null(return_value, "shm_name");
	}
	add_assoc_long(return_value, "valkey_listener", nocopy_vk_listener());
}

/* ---- FrankenPHP worker mode ---- */

/* frankenphp_handle_request() serves many requests inside one worker script, which may keep values across them: copy mode. */
static ZEND_NAMED_FUNCTION(nocopy_handle_request)
{
	NOCOPY_G(zero_copy) = false; /* values fetched while booting stay pinned until the worker ends */
	nocopy_vk_new_request();
	nocopy_ctx *c = &NOCOPY_G(ctx);
	c->transient = c->slot >= 0;
	nocopy_drop_transient(c); /* idle pins: the worker now waits for a request */
	orig_handle_request(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* ---- module ---- */

#if defined(__linux__) && defined(COMPILE_DL_NOCOPY)
#include <link.h>

typedef struct {
	uintptr_t self; /* an address inside our .so */
	uint64_t id;
	bool found;
} build_note;

/* dl_iterate_phdr callback: the GNU build id of the object that contains self (the linker's hash of its contents). */
static int find_build_id(struct dl_phdr_info *info, size_t size, void *arg)
{
	(void) size;
	build_note *b = arg;
	bool ours = false;
	for (int i = 0; i < info->dlpi_phnum && !ours; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		ours = ph->p_type == PT_LOAD && b->self - (info->dlpi_addr + ph->p_vaddr) < ph->p_memsz;
	}
	if (!ours) {
		return 0;
	}
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		if (ph->p_type != PT_NOTE) {
			continue;
		}
		const char *n = (const char *) (info->dlpi_addr + ph->p_vaddr), *end = n + ph->p_memsz;
		while (n + sizeof(ElfW(Nhdr)) <= end) {
			const ElfW(Nhdr) *h = (const ElfW(Nhdr) *) n;
			const char *name = n + sizeof *h, *desc = name + ZEND_MM_ALIGNED_SIZE_EX(h->n_namesz, 4);
			if (h->n_type == NT_GNU_BUILD_ID && h->n_namesz == 4 && memcmp(name, "GNU", 4) == 0 && desc + h->n_descsz <= end) {
				b->id = zend_hash_func(desc, h->n_descsz);
				b->found = true;
				return 1;
			}
			n = desc + ZEND_MM_ALIGNED_SIZE_EX(h->n_descsz, 4);
		}
	}
	return 1;
}
#endif

/* What processes sharing a named arena must agree on: the code that lays out and reads it. The same binary gives the same id; any other build gets its own arena. */
static uint64_t layout_id(void)
{
	uint64_t id = zend_hash_func(ZEND_STRL(PHP_NOCOPY_VERSION " " ZEND_EXTENSION_BUILD_ID));
#ifdef COMPILE_DL_NOCOPY
# ifdef __linux__
	build_note b = {.self = (uintptr_t) layout_id};
	dl_iterate_phdr(find_build_id, &b);
	if (b.found) {
		return id ^ b.id;
	}
# endif
	/* no build id: hash the file itself */
	Dl_info dl;
	int fd = dladdr((void *) layout_id, &dl) && dl.dli_fname ? open(dl.dli_fname, O_RDONLY | O_CLOEXEC) : -1;
	struct stat st;
	void *m = fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	if (fd >= 0) {
		close(fd);
	}
	if (m != MAP_FAILED) {
		id ^= zend_hash_func(m, (size_t) st.st_size);
		munmap(m, (size_t) st.st_size);
		return id;
	}
#endif
	return id ^ zend_hash_func(ZEND_STRL(__DATE__ " " __TIME__)); /* built into PHP, or our .so unreadable */
}

static ZEND_INI_MH(OnShmName)
{
	shm_name = ZSTR_LEN(new_value) ? ZSTR_VAL(new_value) : NULL;
	return SUCCESS;
}

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

static ZEND_INI_MH(OnLocalSize)
{
	local_size = (size_t) zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnHint)
{
	ini_hint = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

static ZEND_INI_MH(OnSerializer)
{
	use_igbinary = zend_string_equals_literal_ci(new_value, "igbinary");
	if (!use_igbinary && !zend_string_equals_literal_ci(new_value, "php") && !zend_string_equals_literal_ci(new_value, "default")) {
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: unknown nocopy.serializer \"%s\", using php", ZSTR_VAL(new_value));
	}
	return SUCCESS;
}

static ZEND_INI_MH(OnTtl)
{
	nocopy_ini_ttl = zend_ini_parse_quantity_warn(new_value, entry->name);
	return SUCCESS;
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY("nocopy.request_scope", "auto", PHP_INI_SYSTEM, OnRequestScope)
	PHP_INI_ENTRY("nocopy.apcu_compat", "0", PHP_INI_SYSTEM, OnBool)
	PHP_INI_ENTRY("nocopy.local_size", "8M", PHP_INI_SYSTEM, OnLocalSize)
	PHP_INI_ENTRY("nocopy.shm_name", "", PHP_INI_SYSTEM, OnShmName)
	PHP_INI_ENTRY_EX("nocopy.valkey", "", PHP_INI_SYSTEM, nocopy_vk_on_url, nocopy_vk_display)
	PHP_INI_ENTRY("nocopy.valkey_prefix", "nocopy:", PHP_INI_SYSTEM, nocopy_vk_on_prefix)
	PHP_INI_ENTRY("nocopy.valkey_timeout", "0.5", PHP_INI_SYSTEM, nocopy_vk_on_timeout)
	PHP_INI_ENTRY1("nocopy.valkey_tls_ca_file", "", PHP_INI_SYSTEM, nocopy_vk_on_path, &nocopy_vk_tls_ca)
	PHP_INI_ENTRY1("nocopy.valkey_tls_cert_file", "", PHP_INI_SYSTEM, nocopy_vk_on_path, &nocopy_vk_tls_cert)
	PHP_INI_ENTRY1("nocopy.valkey_tls_key_file", "", PHP_INI_SYSTEM, nocopy_vk_on_path, &nocopy_vk_tls_key)
	PHP_INI_ENTRY("nocopy.valkey_tls_verify", "1", PHP_INI_SYSTEM, nocopy_vk_on_verify)
PHP_INI_END()

static const zend_ini_entry_def ini_nocopy[] = {
	PHP_INI_ENTRY("nocopy.shm_size", "32M", PHP_INI_SYSTEM, OnShmSize)
	PHP_INI_ENTRY("nocopy.entries_hint", "4096", PHP_INI_SYSTEM, OnHint)
	PHP_INI_ENTRY("nocopy.ttl", "0", PHP_INI_SYSTEM, OnTtl)
	PHP_INI_ENTRY("nocopy.serializer", "php", PHP_INI_SYSTEM, OnSerializer)
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

/* apc.X with the APCu layer: an alias of nocopy.X, used only when set and nocopy.X is not. */
static ZEND_INI_MH(OnAlias)
{
	char canon[32];
	snprintf(canon, sizeof canon, "nocopy.%s", ZSTR_VAL(entry->name) + sizeof("apc.") - 1);
	if (stage != ZEND_INI_STAGE_STARTUP || !configured(ZSTR_VAL(entry->name))) {
		return SUCCESS; /* the default */
	}
	if (configured(canon)) {
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: %s is ignored, %s is set", ZSTR_VAL(entry->name), canon);
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
	PHP_INI_ENTRY("apc.serializer", "php", PHP_INI_SYSTEM, OnAlias)
	{0}
};

/* ini_get() of an alias shows what is in effect. */
static void sync_aliases(void)
{
	static const char *const names[] = {"shm_size", "entries_hint", "ttl", "serializer"};
	for (int i = 0; i < 4; i++) {
		char apc[32], canon[32];
		snprintf(apc, sizeof apc, "apc.%s", names[i]);
		snprintf(canon, sizeof canon, "nocopy.%s", names[i]);
		ini_put(apc, zend_ini_str(canon, strlen(canon), false), false);
	}
}

/* igbinary's C API, found at runtime: no build dependency. ini_get() then shows the serializer in effect. */
static void resolve_serializer(void)
{
	if (use_igbinary) {
		zend_module_entry *m = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("igbinary"));
		void *h = m && m->handle ? m->handle : RTLD_DEFAULT;
		igb_ser = m ? (igb_ser_fn) DL_FETCH_SYMBOL(h, "igbinary_serialize") : NULL;
		igb_unser = m ? (igb_unser_fn) DL_FETCH_SYMBOL(h, "igbinary_unserialize") : NULL;
		if (!igb_ser || !igb_unser) {
			php_error_docref(NULL, E_CORE_WARNING, "nocopy: igbinary is not loaded, using nocopy.serializer=php");
			use_igbinary = false;
		}
	}
	zend_string *v = zend_string_init(use_igbinary ? "igbinary" : "php", use_igbinary ? 8 : 3, 1);
	ini_put("nocopy.serializer", v, false);
	zend_string_release_ex(v, 1);
}

static void atfork_child(void)
{
	if (nocopy_a) {
		nocopy_after_fork(&NOCOPY_G(ctx));
	}
	nocopy_vk_close(&NOCOPY_G(vk));
}

static PHP_GINIT_FUNCTION(nocopy)
{
#if defined(COMPILE_DL_NOCOPY) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(nocopy_globals, 0, sizeof(*nocopy_globals));
	nocopy_globals->ctx.slot = -1;
	nocopy_globals->vk.fd = -1;
	nocopy_globals->keys.slot = pecalloc(NOCOPY_KEYS, sizeof(nocopy_kslot), 1);
}

/* A thread ending (ZTS) gives its worker slot back. */
static PHP_GSHUTDOWN_FUNCTION(nocopy)
{
	if (nocopy_a) {
		nocopy_release(&nocopy_globals->ctx);
	}
	pefree(nocopy_globals->keys.slot, 1);
	nocopy_vk_close(&nocopy_globals->vk);
}

PHP_MINIT_FUNCTION(nocopy)
{
	REGISTER_INI_ENTRIES();
	if (apcu_compat && zend_hash_str_exists(&module_registry, ZEND_STRL("apcu"))) {
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: nocopy.apcu_compat=1, but APCu is loaded: running without the apcu_* layer");
		apcu_compat = false;
	}
	zend_register_ini_entries(ini_nocopy, module_number);
	if (apcu_compat) {
		zend_register_ini_entries(ini_apc, module_number);
	}
	resolve_serializer();
	if (apcu_compat) {
		sync_aliases();
	}
	nocopy_apc_on = ini_enabled && (ini_enable_cli || strcmp(sapi_module.name, "cli") != 0);

	size_t need;
	uint64_t hint = ini_hint > 0 ? (uint64_t) ini_hint : 0;
	const char *err;
	nocopy_named named;
	if (!shm_name) {
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: nocopy.shm_name is not set (e.g. /nocopy). The cache is unavailable in this process: fetches miss, writes fail");
		err = NULL;
	} else if (!(err = nocopy_arena_open(shm_name, arena_size, hint, layout_id(), &need, &named))) {
		memcpy(shm_attached, named.name, sizeof shm_attached);
		if (named.other[0]) {
			php_error_docref(NULL, E_CORE_WARNING, "nocopy: %s, the nocopy.shm_name cache of another PHP or nocopy build, is in use: its processes do not share this cache", named.other);
		}
	} else if (err != NOCOPY_TOO_SMALL) {
		/* not a private cache instead: its writes and deletes would never reach the processes sharing the named one */
		php_error_docref(NULL, E_CORE_WARNING, "nocopy: nocopy.shm_name %s: %s. The cache is unavailable in this process: fetches miss, writes fail", shm_name, err);
		err = NULL;
	}
	const char *p = apcu_compat ? "apc" : "nocopy";
	if (err == NOCOPY_TOO_SMALL) { /* APCu's wording: its tests and users grep for it */
		zend_error(E_CORE_ERROR, "Unable to allocate %zu bytes of shared memory for cache structures. Either %s.shm_size is too small or %s.entries_hint too large", need, p, p);
		return FAILURE;
	}
	if (nocopy_a) {
		arena_empty_init();
		nocopy_vk_minit();
	}
	detour = nocopy_vk_on || !nocopy_a;
	pthread_atfork(NULL, NULL, atfork_child);

	zend_function *hr = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("frankenphp_handle_request"));
	if (hr && hr->type == ZEND_INTERNAL_FUNCTION) {
		orig_handle_request = hr->internal_function.handler;
		hr->internal_function.handler = nocopy_handle_request;
	}
	if (apcu_compat) {
		nocopy_apcu_minit(module_number);
	}
#if PHP_VERSION_ID >= 80400
	/* opcache's file cache stores frameless handler indices: another extension set must not reuse them */
	size_t n = 0; /* zend_flf_count is not exported */
	while (zend_flf_handlers && zend_flf_handlers[n]) {
		n++;
	}
	zend_add_system_entropy("nocopy", "frameless", &n, sizeof(n));
#endif
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(nocopy)
{
	nocopy_release(&NOCOPY_G(ctx));
	nocopy_arena_destroy();
	UNREGISTER_INI_ENTRIES();
	nocopy_vk_mshutdown();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(nocopy)
{
#if defined(COMPILE_DL_NOCOPY) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	if (NOCOPY_G(ctx).slot < 0 && nocopy_a) {
		nocopy_claim(&NOCOPY_G(ctx));
	}
	NOCOPY_G(zero_copy) = request_scope;
	NOCOPY_G(local_ok) = local_size > 0;
	nocopy_vk_new_request();
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(nocopy)
{
	NOCOPY_G(zero_copy) = false;
	NOCOPY_G(local_ok) = false; /* later fetches (session's RSHUTDOWN) copy without keeping */
	if (NOCOPY_G(local)) {
		local_flush();
		efree(NOCOPY_G(local));
		NOCOPY_G(local) = NULL;
	}
	return SUCCESS;
}

/* Pins go only after zend_deactivate: built-in modules (session) still read user values in their RSHUTDOWN, which runs after ours. */
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(nocopy)
{
	while (NOCOPY_G(nleases)) {
		lease_drop(); /* a callback bailed out (fatal, timeout) while holding it */
	}
	NOCOPY_G(ctx).pin_refs = 0;
	nocopy_keep_release(&NOCOPY_G(ctx));
	return SUCCESS;
}

PHP_MINFO_FUNCTION(nocopy)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "nocopy support", "enabled");
#ifdef ZTS
	php_info_print_table_row(2, "thread safety", "enabled");
#endif
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

static const zend_module_dep nocopy_deps[] = {
	ZEND_MOD_OPTIONAL("igbinary") /* starts first, so its symbols resolve in our MINIT */
	ZEND_MOD_END
};

zend_module_entry nocopy_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	nocopy_deps,
	"nocopy",
	ext_functions,
	PHP_MINIT(nocopy),
	PHP_MSHUTDOWN(nocopy),
	PHP_RINIT(nocopy),
	PHP_RSHUTDOWN(nocopy),
	PHP_MINFO(nocopy),
	PHP_NOCOPY_VERSION,
	PHP_MODULE_GLOBALS(nocopy),
	PHP_GINIT(nocopy),
	PHP_GSHUTDOWN(nocopy),
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(nocopy),
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_NOCOPY
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(nocopy)
#endif
