/* nocopy: PHP values in and out of an arena block (size, persist, rebase, copy out, serialize, build). Objects: nocopy_graph.c. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"

bool nocopy_use_igbinary;
nocopy_igb_ser_fn nocopy_igb_ser;
nocopy_igb_unser_fn nocopy_igb_unser;

/* Values that start a value of their own in the key table: strings stored alone bypass it. */
static zend_always_inline void keys_reset_for(nocopy_keys *t, const zval *v)
{
	if (Z_TYPE_P(v) == IS_ARRAY || Z_TYPE_P(v) == IS_OBJECT) {
		keys_reset(t);
	}
}

/* A map's hash in the arena: 2 slots per bucket rounded up, as PHP sizes it, not the table's 16 minimum. */
static zend_always_inline uint32_t map_mask(const zend_array *a)
{
	uint32_t slots = 4;
	while (slots < 2 * a->nNumUsed) {
		slots <<= 1;
	}
	return slots < (uint32_t) -(int32_t) a->nTableMask ? (uint32_t) -(int32_t) slots : a->nTableMask;
}

static zend_always_inline size_t map_size(const zend_array *a) { return HT_HASH_SIZE(map_mask(a)) + a->nNumUsed * sizeof(Bucket); }

/* ---- sizing ---- */

static size_status size_array(sizing *z, zend_array *a, int depth)
{
	if (zend_hash_num_elements(a) == 0) {
		return SIZE_OK;
	}
	z->n += SALIGN(sizeof(zend_array)) + SALIGN(HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : map_size(a));
	size_status st = SIZE_OK;
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if ((st = nocopy_size_zval(z, v, depth + 1)) != SIZE_OK) {
				return st;
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			nocopy_kslot *s;
			if (b->key && !key_seen(&z->keys, b->key, b->h, &s)) {
				z->n += str_size(ZSTR_LEN(b->key));
			}
			if ((st = nocopy_size_zval(z, &b->val, depth + 1)) != SIZE_OK) {
				return st;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return SIZE_OK;
}

size_status nocopy_size_zval(sizing *z, zval *v, int depth)
{
	if (depth > NOCOPY_MAX_DEPTH) {
		return SIZE_DEEP;
	}
	switch (Z_TYPE_P(v)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE:
			return SIZE_OK;
		case IS_REFERENCE:
			if (Z_REFCOUNT_P(v) > 1) {
				return nocopy_size_ref(z, Z_REF_P(v), depth);
			}
			return nocopy_size_zval(z, Z_REFVAL_P(v), depth + 1);
		case IS_STRING:
			size_vstr(z, Z_STR_P(v));
			return SIZE_OK;
		case IS_ARRAY:
			return size_array(z, Z_ARR_P(v), depth);
		case IS_RESOURCE:
			return SIZE_RESOURCE;
		case IS_OBJECT:
			return nocopy_size_object(z, Z_OBJ_P(v), depth);
		default:
			return SIZE_OBJECT;
	}
}

/* Scalars and strings inline: nocopy_size_zval stays out of line since objects made it recursive. */
static zend_always_inline size_status size_root(sizing *z, zval *v)
{
	if (Z_TYPE_P(v) < IS_STRING) {
		return SIZE_OK;
	}
	if (Z_TYPE_P(v) == IS_STRING) { /* no table: it is reset for arrays and objects only */
		z->n += str_size(Z_STRLEN_P(v));
		return SIZE_OK;
	}
	return nocopy_size_zval(z, v, 0);
}

/* ---- persisting ---- */

/* Idempotent: every process writes the same bytes. */
void nocopy_value_minit(void)
{
	nocopy_shared *s = NOCOPY_SHARED;
	memcpy(&s->empty, &zend_empty_array, sizeof(zend_array));
	s->empty.pDestructor = NULL;
	s->empty_hash[0] = s->empty_hash[1] = HT_INVALID_IDX;
	HT_SET_DATA_ADDR(&s->empty, s->empty_hash);
}

zend_string *nocopy_persist_str(persist *p, const zend_string *s)
{
	size_t size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(s));
	zend_string *r = take(p, size);
	memcpy(r, s, size); /* keys arrive hashed; values stay unhashed until used as a key (benign idempotent write) */
	GC_SET_REFCOUNT(r, 2);
	/* not IS_STR_PERMANENT: that promises process lifetime (PCRE then keeps the pointer as a cache key) */
	GC_TYPE_INFO(r) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	return r;
}

static zend_always_inline zend_string *persist_vstr(persist *p, zend_string *s)
{
	if (ZSTR_LEN(s) > NOCOPY_VSTR_MAX) {
		return nocopy_persist_str(p, s);
	}
	nocopy_kslot *k;
	return key_seen(&p->keys, s, zend_string_hash_val(s), &k) ? k->copy : (k->copy = nocopy_persist_str(p, s));
}

/* The table's data (hash and buckets) into the block; a map's hash shrunk to map_mask, its buckets rechained as opcache compacts. */
static void persist_data(persist *p, zend_array *ht, const zend_array *src)
{
	if (HT_IS_PACKED(src) || (ht->nTableMask = map_mask(src)) == src->nTableMask) {
		size_t size = HT_IS_PACKED(src) ? HT_PACKED_USED_SIZE(src) : HT_USED_SIZE(src);
		void *data = take(p, size);
		memcpy(data, HT_GET_DATA_ADDR(src), size);
		HT_SET_DATA_ADDR(ht, data);
		return;
	}
	HT_SET_DATA_ADDR(ht, take(p, map_size(src)));
	Bucket *d = ht->arData;
	const Bucket *sb = src->arData;
	uint32_t mask = ht->nTableMask;
	for (uint32_t *h = (uint32_t *) d - (uint32_t) -(int32_t) mask; h < (uint32_t *) d; h++) {
		*h = HT_INVALID_IDX;
	}
	for (uint32_t i = 0; i < ht->nNumUsed; i++) {
		d[i] = sb[i];
		if (EXPECTED(Z_TYPE(d[i].val) != IS_UNDEF)) {
			uint32_t ni = (uint32_t) d[i].h | mask;
			Z_NEXT(d[i].val) = HT_HASH_EX(d, ni);
			HT_HASH_EX(d, ni) = HT_IDX_TO_HASH(i);
		}
	}
}

static zend_array *persist_array(persist *p, const zend_array *src)
{
	if (zend_hash_num_elements(src) == 0) {
		return ARENA_EMPTY;
	}
	zend_array *ht = take(p, sizeof(zend_array));
	memcpy(ht, src, sizeof(zend_array));
	ht->pDestructor = NULL; /* a function address: differs between processes; immutable arrays are never destroyed */
	persist_data(p, ht, src);
	ht->nInternalPointer = 0;
	GC_SET_REFCOUNT(ht, 2);
	GC_TYPE_INFO(ht) = GC_ARRAY | IS_ARRAY_IMMUTABLE;

	/* serialization does not keep the next free index, so neither do we: max int key + 1 */
	zend_long max = ZEND_LONG_MIN;
	if (HT_IS_PACKED(ht)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(ht, v) {
			nocopy_persist_zval(p, v);
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
			nocopy_kslot *s;
			if (b->key) {
				b->key = key_seen(&p->keys, b->key, b->h, &s) ? s->copy : (s->copy = nocopy_persist_str(p, b->key));
			} else if ((zend_long) b->h > max) {
				max = (zend_long) b->h;
			}
			nocopy_persist_zval(p, &b->val);
		} ZEND_HASH_FOREACH_END();
	}
	ht->nNextFreeElement = (max == ZEND_LONG_MIN || max == ZEND_LONG_MAX) ? max : max + 1;
	return ht;
}

void nocopy_persist_zval(persist *p, zval *z)
{
	switch (Z_TYPE_P(z)) {
		case IS_REFERENCE:
			if (p->g && Z_REFCOUNT_P(z) > 1 && nocopy_persist_ref(p, z)) {
				break;
			}
			ZVAL_COPY_VALUE(z, Z_REFVAL_P(z));
			nocopy_persist_zval(p, z);
			break;
		case IS_STRING:
			Z_STR_P(z) = persist_vstr(p, Z_STR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			break;
		case IS_ARRAY: {
			bool outer = p->obj;
			p->obj = false;
			Z_ARR_P(z) = persist_array(p, Z_ARR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			if (p->obj) {
				Z_TYPE_INFO_P(z) = NC_SPINE;
			}
			p->obj |= outer;
			break;
		}
		case IS_OBJECT:
			nocopy_persist_object(p, z);
			break;
		default: /* scalars: sizing already refused everything else */
			break;
	}
}

/* Fills e after its header: the key's bytes (at KEY_OFF), then the value. */
static void persist_entry(nocopy_entry *e, const zend_string *key, zval *val, nocopy_keys *keys, graph_b *g)
{
	keys_reset_for(keys, val);
	persist p = {.cursor = (char *) e + SALIGN(sizeof(php_entry)), .keys = *keys, .g = g};
	memcpy(take(&p, ZSTR_LEN(key)), ZSTR_VAL(key), ZSTR_LEN(key));
	if (g) {
		ZVAL_PTR(entry_val(e), nocopy_persist_graph(&p, val));
	} else if (Z_TYPE_P(val) == IS_STRING) { /* as size_root: not through the table */
		ZVAL_STR(entry_val(e), nocopy_persist_str(&p, Z_STR_P(val)));
		Z_TYPE_FLAGS_P(entry_val(e)) = 0;
	} else {
		ZVAL_COPY_VALUE(entry_val(e), val);
		nocopy_persist_zval(&p, entry_val(e));
	}
	ZEND_ASSERT(p.cursor == (char *) e + e->size);
}

/* ---- moving ---- */

void nocopy_rebase_zval(void *ctx, zval *z)
{
	rebase *r = ctx;
	switch (Z_TYPE_P(z)) {
		case IS_STRING:
			Z_STR_P(z) = moved(r, Z_STR_P(z));
			ZSTR_H(Z_STR_P(z)) = 0; /* readers may have been hashing the old copy while it was copied: rehash on use */
			break;
		case NC_SPINE: /* only in graphs */
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(z) = moved(r, Z_ARR_P(z));
			if (a == ARENA_EMPTY) {
				break;
			}
			a->arData = moved(r, a->arData);
			if (HT_IS_PACKED(a)) {
				zval *v;
				ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
					nocopy_rebase_zval(r, v);
				} ZEND_HASH_FOREACH_END();
			} else {
				Bucket *b;
				ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
					if (b->key) {
						b->key = moved(r, b->key);
					}
					nocopy_rebase_zval(r, &b->val);
				} ZEND_HASH_FOREACH_END();
			}
			break;
		}
	}
}

void nocopy_rebase(nocopy_entry *e, const nocopy_entry *from)
{
	rebase r = {.from = (uintptr_t) from, .size = from->size, .by = (char *) e - (const char *) from};
	if (e->flags & ENTRY_GRAPH) {
		nocopy_graph_rebase(entry_val(e), &r);
	} else {
		nocopy_rebase_zval(&r, entry_val(e));
	}
}

/* ---- leaving shared memory ---- */

void nocopy_copy_into(nocopy_keys *keys, zval *dst, const zval *src)
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
			/* bulk-copy the buckets, then repoint whatever still lives in the arena (stored values are at most NOCOPY_MAX_DEPTH deep) */
			ZVAL_ARR(dst, zend_array_dup(a));
			array_each(Z_ARR_P(dst), keys, nocopy_unshare_zval, keys);
			break;
		}
		default:
			ZVAL_COPY_VALUE(dst, src);
	}
}

void nocopy_unshare_key(nocopy_keys *keys, zend_array *a, Bucket *b)
{
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

void nocopy_unshare_zval(void *keys, zval *z)
{
	if ((Z_TYPE_P(z) == IS_STRING || Z_TYPE_P(z) == IS_ARRAY) && in_arena(Z_COUNTED_P(z))) {
		zval t;
		nocopy_copy_into(keys, &t, z);
		ZVAL_COPY_VALUE(z, &t);
	}
}

void nocopy_copy_out(zval *dst, const zval *src)
{
	nocopy_keys *keys = &NOCOPY_G(keys);
	if (Z_TYPE_P(src) == IS_ARRAY) {
		keys_reset(keys);
	}
	nocopy_copy_into(keys, dst, src);
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

/* Sizes the value: SIZE_OK with z->n, the bytes after the key; g holds its objects and shared references if it has any.
 * Sizing runs no user code: __serialize/__sleep run between passes, so the last pass sees what persisting will. */
static size_status size_value(sizing *z, zval *val, nocopy_keys *keys)
{
	size_status st = size_root(z, val);
	for (int pass = 0; st == SIZE_OK && graph_b_pending(z->g); pass++) {
		if (pass == NOCOPY_MAX_DEPTH || (st = nocopy_graph_b_hooks(z->g)) != SIZE_OK) {
			st = st == SIZE_OK ? SIZE_DEEP : st;
			break;
		}
		keys_reset(keys); /* a hook may have stored */
		z->keys = *keys;
		z->n = 0;
		nocopy_graph_b_rewind(z->g);
		st = size_root(z, val);
	}
	if (st == SIZE_OK && graph_b_used(z->g)) {
		z->n += nocopy_graph_head(z->g);
	}
	return st;
}

nocopy_status nocopy_build(nocopy_put *p, zend_string *key, zval *val, zend_long ttl, uint32_t user, bool add_only)
{
	nocopy_keys *keys = &NOCOPY_G(keys);
	keys_reset_for(keys, val);
	graph_b g;
	graph_b_off(&g);
	sizing z = {.keys = *keys, .g = &g};
	zval ser;
	bool serialized = false, igb = false;
	size_status st = size_value(&z, val, keys);
	bool graph = st == SIZE_OK && graph_b_used(&g);
	if (!graph) { /* nothing to persist from it: references held once only, or a value to serialize */
		nocopy_graph_b_free(&g);
	}
	if (st != SIZE_OK) {
		if (st == SIZE_ERROR) {
			return NOCOPY_REFUSED;
		}
		if (st == SIZE_RESOURCE) {
			php_error_docref(NULL, E_WARNING, "nocopy: cannot store resources");
			return NOCOPY_REFUSED;
		}
		if (!nocopy_serialize(val, &ser, &igb)) {
			return NOCOPY_REFUSED;
		}
		val = &ser;
		serialized = true;
		z.n = str_size(Z_STRLEN(ser));
	}

	size_t size = SALIGN(sizeof(php_entry)) + SALIGN(ZSTR_LEN(key)) + z.n;
	nocopy_entry *e = !p ? NULL : nocopy_put_begin(p, &NOCOPY_G(ctx), zend_string_hash_val(key), (uint32_t) ZSTR_LEN(key), KEY_OFF, size, ttl);
	nocopy_status r = NOCOPY_NO_ROOM;
	if (e) {
		e->flags = serialized ? ENTRY_SERIALIZED | (igb ? ENTRY_IGBINARY : 0) : graph ? ENTRY_GRAPH : 0;
		e->user = user;
		persist_entry(e, key, val, keys, graph ? &g : NULL);
		nocopy_put_result c = nocopy_put_commit(p, add_only);
		r = c == PUT_OK ? NOCOPY_OK : c == PUT_EXISTS ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
	}
	if (graph) { /* after the commit: freeing the hooks' results may run destructors */
		nocopy_graph_b_free(&g);
	}
	if (serialized) {
		zend_string_release(Z_STR(ser));
	}
	return r;
}
