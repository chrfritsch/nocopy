/* nocopy: PHP values in and out of an arena block (size, persist, rebase, copy out, serialize). */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"

#define NOCOPY_MAX_DEPTH 64

bool nocopy_use_igbinary;
nocopy_igb_ser_fn nocopy_igb_ser;
nocopy_igb_unser_fn nocopy_igb_unser;

/* ---- the key table: equal array keys stored once per value ---- */

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
void nocopy_value_minit(void)
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

/* Fills e after its header: the key's bytes (at KEY_OFF), then the value. */
static void persist_entry(nocopy_entry *e, const zend_string *key, zval *val, nocopy_keys *keys)
{
	if (Z_TYPE_P(val) == IS_ARRAY) {
		keys_reset(keys);
	}
	persist p = {.cursor = (char *) e + SALIGN(sizeof(php_entry)), .keys = *keys};
	memcpy(take(&p, ZSTR_LEN(key)), ZSTR_VAL(key), ZSTR_LEN(key));
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


void nocopy_copy_out(zval *dst, const zval *src)
{
	copy_out(&NOCOPY_G(keys), dst, src);
}

/* ---- serializing ---- */

bool nocopy_serialize(zval *val, zval *out, bool *igb)
{
	*igb = nocopy_use_igbinary;
	if (nocopy_use_igbinary) {
		uint8_t *b;
		size_t len;
		if (nocopy_igb_ser(&b, &len, val) != 0 || EG(exception)) {
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

/* Unserialize with a fresh state: php_unserialize_with_options() would share the outer
 * unserialize()'s reference table when called re-entrantly (e.g. from Serializable::unserialize). */
bool nocopy_unserialize(zval *rv, const char *buf, size_t len, uint8_t flags)
{
	bool ok;
	if (flags & ENTRY_IGBINARY) {
		ZVAL_NULL(rv);
		ok = nocopy_igb_unser && nocopy_igb_unser((const uint8_t *) buf, len, rv) == 0; /* no igb_unser: a miss */
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

/* ---- building an entry ---- */

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

	size_t size = SALIGN(sizeof(php_entry)) + SALIGN(ZSTR_LEN(key)) + z.n;
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
