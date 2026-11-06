/* nocopy: zero-copy shared-memory store. This file: values in and out of the arena, nocopy_*, the module. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#if PHP_VERSION_ID < 80400
# error "nocopy needs PHP 8.4 or later"
#endif
#include "nocopy_arena.h"
#include "ext/standard/info.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "SAPI.h"
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
static char *shm_name; /* nocopy.shm_name: required, the arena every process attaches to */
static bool detour; /* Valkey on, or no arena: operations leave the local fast path (one check on it, set in MINIT) */
static char shm_attached[64]; /* the object mapped; empty: none (nocopy_a NULL: unavailable) */
static bool sapi_zero_copy;
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

/* nocopy_entry.user: nocopy_remember's compute time, ms (XFetch) */

#define KEY_OFF (SALIGN(sizeof(php_entry)) + _ZSTR_HEADER_SIZE) /* the key string comes first after the zval */
_Static_assert(KEY_OFF <= UINT8_MAX, "koff is 8 bits");

static inline zval *entry_val(nocopy_entry *e) { return &((php_entry *) e)->val; }

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
			if (Z_REFCOUNT_P(v) > 1) {
				return SIZE_OBJECT; /* must stay shared: serialize */
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

	/* serialization does not keep the next free index, so neither do we: max int key + 1 */
	zend_long max = ZEND_LONG_MIN;
	if (HT_IS_PACKED(ht)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(ht, v) {
			persist_zval(p, v);
		} ZEND_HASH_FOREACH_END();
		for (uint32_t i = ht->nNumUsed; i-- > 0;) {
			if (Z_TYPE(ht->arPacked[i]) != IS_UNDEF) {
				max = (zend_long) i;
				break;
			}
		}
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(ht, b) {
			if (b->key) {
				nocopy_kslot *s = key_slot(&p->keys, b->h);
				if (key_hit(&p->keys, s, b->key, b->h)) { /* s->s: the source key, so equal pointers skip the compare */
					b->key = s->copy;
				} else {
					key_put(&p->keys, s, b->key, b->h);
					b->key = s->copy = persist_str(p, b->key);
				}
			} else if ((zend_long) b->h > max) {
				max = (zend_long) b->h;
			}
			persist_zval(p, &b->val);
		} ZEND_HASH_FOREACH_END();
	}
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
			ZVAL_STRINGL_FAST(dst, Z_STRVAL_P(src), Z_STRLEN_P(src)); /* "" and 1 char: interned, no alloc */
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
			zval_ptr_dtor(&NOCOPY_G(local)[i].v);
			NOCOPY_G(local)[i].e = NULL;
		}
	}
	NOCOPY_G(local_bytes) = 0;
}

static void local_drop(nocopy_local *l)
{
	NOCOPY_G(local_bytes) -= l->size;
	zval_ptr_dtor(&l->v);
	l->e = NULL;
}

/* Over budget: SIEVE evicts copies not used since the hand last passed (2 rounds at most: the first clears every bit). */
static void local_put(nocopy_local *l, const nocopy_entry *e, const zval *copy)
{
	if (l->e) {
		local_drop(l);
	}
	for (uint32_t n = 0; NOCOPY_G(local_bytes) + e->size > local_size && n < 2 * NOCOPY_LOCAL; n++) {
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

/* ---- writes ---- */

bool nocopy_serialize(zval *val, zval *out, bool *igb)
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

/* Sizes the value, allocates and fills its entry. NOCOPY_OK: p holds it, ready for nocopy_publish().
 * p NULL (no arena): only the checks, so a value a store would refuse is refused alike; then NOCOPY_NO_ROOM. */
nocopy_status nocopy_build(nocopy_put *p, zend_string *key, zval *val, const nocopy_opts *o)
{
	nocopy_keys *keys = &NOCOPY_G(keys);
	if (Z_TYPE_P(val) == IS_ARRAY) {
		keys_reset(keys);
	}
	sizing z = {.keys = *keys};
	zval ser;
	bool serialized = false, igb = false;
	size_status st = size_zval(&z, val, 0);
	if (st != SIZE_OK) {
		if (st == SIZE_RESOURCE) {
			php_error_docref(NULL, E_WARNING, "nocopy: cannot store resources");
			return NOCOPY_REFUSED;
		}
		if (!nocopy_serialize(val, &ser, &igb)) {
			return NOCOPY_REFUSED;
		}
		val = &ser;
		serialized = true;
		z.n = SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN(ser)));
	}

	size_t size = SALIGN(sizeof(php_entry)) + SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(key))) + z.n;
	nocopy_entry *e = !p ? NULL : nocopy_put_begin(p, &NOCOPY_G(ctx), zend_string_hash_val(key), (uint32_t) ZSTR_LEN(key), KEY_OFF, size,
		o->ttl);
	if (e) {
		e->flags = serialized ? ENTRY_SERIALIZED | (igb ? ENTRY_IGBINARY : 0) : 0;
		persist_entry(e, key, val, keys);
	}
	if (serialized) {
		zend_string_release(Z_STR(ser));
	}
	return e ? NOCOPY_OK : NOCOPY_NO_ROOM;
}

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
	if (UNEXPECTED(detour)) {
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
	if (UNEXPECTED(detour)) {
		return nocopy_a && nocopy_vk_drop(key);
	}
	return drop_local(ZSTR_VAL(key), ZSTR_LEN(key));
}

zend_long nocopy_drop_multi(HashTable *keys)
{
	if (UNEXPECTED(detour)) {
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
	if (UNEXPECTED(detour)) {
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
	if (UNEXPECTED(detour)) {
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

/* Unserialize with a fresh state: php_unserialize_with_options() would share the outer
 * unserialize()'s reference table when called re-entrantly (e.g. from Serializable::unserialize). */
bool nocopy_unserialize(zval *rv, const char *buf, size_t len, uint8_t flags)
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
		if (e->flags & ENTRY_SERIALIZED) {
			hit = nocopy_unserialize(rv, Z_STRVAL_P(v), Z_STRLEN_P(v), e->flags);
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

static zend_always_inline bool local_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	return local_fetch_ex(key, rv, exp, delta, false);
}

bool nocopy_fetch_local(zend_string *key, zval *rv, time_t *exp, uint32_t *delta) { return local_fetch(key, rv, exp, delta); }

static zend_always_inline bool fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta)
{
	if (UNEXPECTED(detour)) {
		if (!nocopy_a) {
			ZVAL_NULL(rv);
			return false;
		}
		return nocopy_vk_fetch(key, rv, exp, delta);
	}
	return local_fetch(key, rv, exp, delta);
}

bool nocopy_fetch(zend_string *key, zval *rv) { return fetch(key, rv, NULL, NULL); }

void nocopy_fetch_multi(HashTable *keys, HashTable *out)
{
	if (UNEXPECTED(detour)) {
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
		if (beta > 0 && !detour && local_fetch_ex(key, rv, NULL, NULL, true)) {
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
	compute(key, fci, fcc, o, rv);
	if (mine) {
		lease_drop();
	}
}

/* ---- nocopy_* ---- */

static void put_impl(zend_string *key, zval *val, zend_long ttl, bool add_only, zval *return_value)
{
	if (ttl < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl, .add_only = add_only};
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

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(val)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	put_impl(key, val, ttl, false, return_value);
}

PHP_FUNCTION(nocopy_add)
{
	zend_string *key;
	zval *val;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(key)
		Z_PARAM_ZVAL(val)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	put_impl(key, val, ttl, true, return_value);
}

PHP_FUNCTION(nocopy_inc)
{
	zend_string *key;
	zend_long step = 1, ttl = 0, nv;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_STR(key)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(step)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	if (!nocopy_add_long(key, step, ttl, &nv)) {
		RETURN_FALSE;
	}
	RETURN_LONG(nv);
}

PHP_FUNCTION(nocopy_cas)
{
	zend_string *key;
	zend_long old, nw;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_STR(key)
		Z_PARAM_LONG(old)
		Z_PARAM_LONG(nw)
	ZEND_PARSE_PARAMETERS_END();

	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	RETURN_BOOL(nocopy_cas_long(key, old, nw));
}

PHP_FUNCTION(nocopy_clear)
{
	ZEND_PARSE_PARAMETERS_NONE();
	nocopy_clear_all();
	RETURN_TRUE;
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

PHP_FUNCTION(nocopy_delete)
{
	zend_string *key;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(key)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(nocopy_drop(key));
}

PHP_FUNCTION(nocopy_delete_multiple)
{
	HashTable *keys;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY_HT(keys)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_LONG(nocopy_drop_multi(keys));
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
	array_init_size(return_value, zend_hash_num_elements(keys));
	nocopy_fetch_multi(keys, Z_ARRVAL_P(return_value));
}

PHP_FUNCTION(nocopy_put_multiple)
{
	HashTable *vals;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ARRAY_HT(vals)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();

	if (ttl < 0) {
		zend_argument_value_error(2, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (!nocopy_tracked()) {
		RETURN_THROWS();
	}
	const nocopy_opts o = {.ttl = ttl};
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
	const nocopy_opts o = {.ttl = ttl};
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

/* Zero-copy only under SAPIs whose requests are short: values cannot outlive the request. */
static bool short_lived_sapi(void)
{
	static const char *const names[] = {"fpm-fcgi", "cgi-fcgi", "apache2handler", "apache2filter", "litespeed", "fcgi", "cli-server", "frankenphp", NULL};
	for (int i = 0; names[i]; i++) {
		if (strcmp(sapi_module.name, names[i]) == 0) {
			return true;
		}
	}
	return false;
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

PHP_INI_BEGIN()
	PHP_INI_ENTRY("nocopy.shm_size", "32M", PHP_INI_SYSTEM, OnShmSize)
	PHP_INI_ENTRY("nocopy.entries_hint", "4096", PHP_INI_SYSTEM, OnHint)
	PHP_INI_ENTRY("nocopy.serializer", "php", PHP_INI_SYSTEM, OnSerializer)
	PHP_INI_ENTRY("nocopy.local_size", "8M", PHP_INI_SYSTEM, OnLocalSize)
	PHP_INI_ENTRY("nocopy.shm_name", "", PHP_INI_SYSTEM, OnShmName)
#ifdef HAVE_NOCOPY_VALKEY
	PHP_INI_ENTRY_EX("nocopy.valkey", "", PHP_INI_SYSTEM, nocopy_vk_on_url, nocopy_vk_display)
	PHP_INI_ENTRY("nocopy.valkey_prefix", "nocopy:", PHP_INI_SYSTEM, nocopy_vk_on_prefix)
	PHP_INI_ENTRY("nocopy.valkey_timeout", "0.5", PHP_INI_SYSTEM, nocopy_vk_on_timeout)
	PHP_INI_ENTRY1("nocopy.valkey_tls_ca_file", "", PHP_INI_SYSTEM, nocopy_vk_on_path, &nocopy_vk_tls_ca)
	PHP_INI_ENTRY1("nocopy.valkey_tls_cert_file", "", PHP_INI_SYSTEM, nocopy_vk_on_path, &nocopy_vk_tls_cert)
	PHP_INI_ENTRY1("nocopy.valkey_tls_key_file", "", PHP_INI_SYSTEM, nocopy_vk_on_path, &nocopy_vk_tls_key)
	PHP_INI_ENTRY("nocopy.valkey_tls_verify", "1", PHP_INI_SYSTEM, nocopy_vk_on_verify)
#endif
PHP_INI_END()

/* Read by apcu_* callers (Symfony's ApcuAdapter, Composer) before they use the apcu polyfill; nocopy itself ignores them. */
static const zend_ini_entry_def ini_apc[] = {
	PHP_INI_ENTRY("apc.enabled", "1", PHP_INI_SYSTEM, NULL)
	PHP_INI_ENTRY("apc.enable_cli", "0", PHP_INI_SYSTEM, NULL)
	{0}
};

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
	sapi_zero_copy = short_lived_sapi();
	if (!zend_hash_str_exists(&module_registry, ZEND_STRL("apcu"))) {
		zend_register_ini_entries(ini_apc, module_number);
	}
	resolve_serializer();

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
	if (err == NOCOPY_TOO_SMALL) {
		zend_error(E_CORE_ERROR, "nocopy: %zu bytes needed for the slot tables and a few segments: nocopy.shm_size is too small or nocopy.entries_hint too large", need);
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
	/* opcache's file cache stores frameless handler indices: another extension set must not reuse them */
	size_t n = 0; /* zend_flf_count is not exported */
	while (zend_flf_handlers && zend_flf_handlers[n]) {
		n++;
	}
	zend_add_system_entropy("nocopy", "frameless", &n, sizeof(n));
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
	NOCOPY_G(zero_copy) = sapi_zero_copy;
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
