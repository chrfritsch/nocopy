/* nocopy: PHP values in and out of an arena block (size, persist, rebase, copy out, serialize). */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "zend_lazy_objects.h"

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

/* ---- object graphs: plain user objects stored as their raw property slots ---- */

#define NC_OREF IS_OBJECT /* in a graph: Z_LVAL is the object's index */
#define NC_SPINE IS_PTR   /* in a graph: an array with objects below it, copied on fetch */

typedef struct {
	zend_string *name;
	zend_string *lc; /* hashed, for the class table */
	uint64_t fp;
	uint32_t nprops;
} nc_class;

typedef struct {
	uint32_t cls;
	zval props[];
} nc_obj;

/* Followed by nc_class[ncls], then uint32_t off[nobj]: each object's offset from the graph. */
typedef struct {
	uint32_t ncls, nobj;
	zval root;
} nc_graph;

static zend_always_inline nc_class *graph_cls(const nc_graph *g) { return (nc_class *) (g + 1); }
static zend_always_inline uint32_t *graph_off(const nc_graph *g) { return (uint32_t *) (graph_cls(g) + g->ncls); }
static zend_always_inline nc_obj *graph_obj(const nc_graph *g, uint32_t i) { return (nc_obj *) ((char *) g + graph_off(g)[i]); }
static zend_always_inline size_t graph_head(uint32_t ncls, uint32_t nobj) { return sizeof(nc_graph) + ncls * sizeof(nc_class) + nobj * sizeof(uint32_t); }
static zend_always_inline size_t obj_size(uint32_t nprops) { return offsetof(nc_obj, props) + nprops * sizeof(zval); }

/* 0: not storable as slots (internal, custom serialization, ...). Else a hash of the slot layout: names, visibility, types. */
static uint64_t class_fp_compute(zend_class_entry *ce)
{
	if (ce->type != ZEND_USER_CLASS || ce->create_object || ce->default_object_handlers != &std_object_handlers
		|| (ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS
			| ZEND_ACC_ENUM | ZEND_ACC_ANON_CLASS | ZEND_ACC_NOT_SERIALIZABLE))
		|| ce->__serialize || ce->__unserialize || ce->serialize || ce->unserialize
		|| zend_hash_str_exists(&ce->function_table, ZEND_STRL("__sleep")) || zend_hash_str_exists(&ce->function_table, ZEND_STRL("__wakeup"))) {
		return 0;
	}
	smart_str b = {0};
	smart_str_append(&b, ce->name);
	smart_str_append_long(&b, ce->default_properties_count);
	for (int i = 0; i < ce->default_properties_count; i++) {
		zend_property_info *pi = ce->properties_info_table ? ce->properties_info_table[i] : NULL;
		if (!pi) {
			smart_str_free(&b);
			return 0;
		}
		smart_str_appendc(&b, 0);
		smart_str_append(&b, pi->name);
		smart_str_append_long(&b, pi->flags & (ZEND_ACC_PPP_MASK | ZEND_ACC_PPP_SET_MASK | ZEND_ACC_READONLY));
		if (ZEND_TYPE_IS_SET(pi->type)) {
			zend_string *t = zend_type_to_string(pi->type);
			smart_str_append(&b, t);
			zend_string_release(t);
		}
	}
	smart_str_0(&b);
	uint64_t fp = zend_hash_func(ZSTR_VAL(b.s), ZSTR_LEN(b.s)) | 1;
	smart_str_free(&b);
	return fp;
}

static uint64_t class_fp(zend_class_entry *ce)
{
	HashTable *t = &NOCOPY_G(classes);
	if (!NOCOPY_G(classes_on)) {
		zend_hash_init(t, 16, NULL, NULL, 0);
		NOCOPY_G(classes_on) = true;
	}
	zval *f = zend_hash_index_find(t, (zend_ulong) (uintptr_t) ce);
	if (f) {
		return (uint64_t) Z_LVAL_P(f);
	}
	zval z;
	ZVAL_LONG(&z, (zend_long) class_fp_compute(ce));
	zend_hash_index_add_new(t, (zend_ulong) (uintptr_t) ce, &z);
	return (uint64_t) Z_LVAL(z);
}

void nocopy_classes_clear(void)
{
	if (NOCOPY_G(classes_on)) {
		zend_hash_destroy(&NOCOPY_G(classes));
		NOCOPY_G(classes_on) = false;
	}
}

/* One value's objects and classes, in the order sizing meets them (persisting walks the same order). */
typedef struct {
	HashTable objs; /* handle -> index */
	HashTable cls;  /* class entry -> index */
	bool on;
} graph_b;

static void graph_b_free(graph_b *g)
{
	if (g->on) {
		zend_hash_destroy(&g->objs);
		zend_hash_destroy(&g->cls);
	}
}

/* ---- sizing ---- */

typedef enum { SIZE_OK, SIZE_DEEP, SIZE_OBJECT, SIZE_RESOURCE } size_status;

typedef struct {
	size_t n;              /* out-of-line bytes persist_zval() will consume */
	nocopy_keys keys;       /* this value's view of the table (after keys_reset) */
	graph_b *g;
} sizing;

static size_status size_zval(sizing *z, zval *v, int depth);
static size_status size_object(sizing *z, zend_object *o, int depth);

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
		case IS_OBJECT:
			return size_object(z, Z_OBJ_P(v), depth);
		default:
			return SIZE_OBJECT;
	}
}

/* Scalars and strings inline: size_zval stays out of line since objects made it recursive. */
static zend_always_inline size_status size_root(sizing *z, zval *v)
{
	if (Z_TYPE_P(v) < IS_STRING) {
		return SIZE_OK;
	}
	if (Z_TYPE_P(v) == IS_STRING) {
		z->n += SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN_P(v)));
		return SIZE_OK;
	}
	return size_zval(z, v, 0);
}

static bool has_dynamic(const HashTable *props)
{
	zval *v;
	ZEND_HASH_FOREACH_VAL(props, v) {
		if (Z_TYPE_P(v) != IS_INDIRECT) {
			return true;
		}
	} ZEND_HASH_FOREACH_END();
	return false;
}

/* SIZE_OBJECT: not a plain object, so the whole value is serialized. */
__attribute__((noinline)) static size_status size_object(sizing *z, zend_object *o, int depth)
{
	graph_b *g = z->g;
	if (!g->on) {
		zend_hash_init(&g->objs, 8, NULL, NULL, 0);
		zend_hash_init(&g->cls, 4, NULL, NULL, 0);
		g->on = true;
	}
	zval idx;
	ZVAL_LONG(&idx, zend_hash_num_elements(&g->objs));
	if (!zend_hash_index_add(&g->objs, o->handle, &idx)) {
		return SIZE_OK; /* met before: a back reference */
	}
	zend_class_entry *ce = o->ce;
	if (o->handlers != &std_object_handlers || zend_object_is_lazy(o) || (o->properties && has_dynamic(o->properties))) {
		return SIZE_OBJECT;
	}
	if (!zend_hash_index_exists(&g->cls, (zend_ulong) (uintptr_t) ce)) {
		if (!class_fp(ce)) {
			return SIZE_OBJECT;
		}
		ZVAL_LONG(&idx, zend_hash_num_elements(&g->cls));
		zend_hash_index_add_new(&g->cls, (zend_ulong) (uintptr_t) ce, &idx);
		z->n += 2 * SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(ce->name)));
	}
	z->n += SALIGN(obj_size(ce->default_properties_count));
	for (int i = 0; i < ce->default_properties_count; i++) {
		size_status st = size_zval(z, OBJ_PROP_NUM(o, i), depth + 1);
		if (st != SIZE_OK) {
			return st;
		}
	}
	return SIZE_OK;
}

/* ---- persisting: values into an arena block, opcache-style immutable ---- */

typedef struct {
	char *cursor; /* next free byte of the block */
	nocopy_keys keys;
	graph_b *g;
	nc_graph *graph;
	bool *done; /* objects persisted so far */
	bool obj;   /* an object met since the innermost array began */
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

/* An object becomes NC_OREF; its slots go into a node the first time it is met. */
__attribute__((noinline)) static void persist_object(persist *p, zval *z)
{
	zend_object *o = Z_OBJ_P(z);
	uint32_t i = (uint32_t) Z_LVAL_P(zend_hash_index_find(&p->g->objs, o->handle));
	if (!p->done[i]) { /* NOLINT(clang-analyzer-core.NullDereference): objects are met only in graphs */
		p->done[i] = true;
		zend_class_entry *ce = o->ce;
		nc_obj *n = take(p, obj_size(ce->default_properties_count));
		graph_off(p->graph)[i] = (uint32_t) ((char *) n - (char *) p->graph);
		n->cls = (uint32_t) Z_LVAL_P(zend_hash_index_find(&p->g->cls, (zend_ulong) (uintptr_t) ce));
		for (int k = 0; k < ce->default_properties_count; k++) {
			ZVAL_COPY_VALUE(&n->props[k], OBJ_PROP_NUM(o, k));
			persist_zval(p, &n->props[k]);
		}
	}
	Z_TYPE_INFO_P(z) = NC_OREF;
	Z_LVAL_P(z) = i;
	p->obj = true;
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
			persist_object(p, z);
			break;
		default: /* scalars: sizing already refused everything else */
			break;
	}
}

__attribute__((noinline)) static nc_graph *persist_graph(persist *p, zval *val)
{
	graph_b *g = p->g;
	uint32_t ncls = zend_hash_num_elements(&g->cls), nobj = zend_hash_num_elements(&g->objs);
	nc_graph *gr = take(p, graph_head(ncls, nobj));
	gr->ncls = ncls;
	gr->nobj = nobj;
	nc_class *c = graph_cls(gr);
	zend_ulong k;
	ZEND_HASH_FOREACH_NUM_KEY(&g->cls, k) {
		zend_class_entry *ce = (zend_class_entry *) (uintptr_t) k;
		zend_string *lc = zend_string_tolower(ce->name); /* NOLINT(clang-analyzer-core.NullDereference): keys are class entries */
		zend_string_hash_val(lc);
		c->name = persist_str(p, ce->name);
		c->lc = persist_str(p, lc);
		zend_string_release(lc);
		c->fp = class_fp(ce);
		c->nprops = (uint32_t) ce->default_properties_count;
		c++;
	} ZEND_HASH_FOREACH_END();
	p->graph = gr;
	p->done = ecalloc(nobj, sizeof(bool));
	ZVAL_COPY_VALUE(&gr->root, val);
	persist_zval(p, &gr->root);
	efree(p->done);
	return gr;
}

/* Fills e after its header: the key's bytes (at KEY_OFF), then the value. */
static void persist_entry(nocopy_entry *e, const zend_string *key, zval *val, nocopy_keys *keys, graph_b *g)
{
	if (Z_TYPE_P(val) == IS_ARRAY || Z_TYPE_P(val) == IS_OBJECT) {
		keys_reset(keys);
	}
	persist p = {.cursor = (char *) e + SALIGN(sizeof(php_entry)), .keys = *keys, .g = g};
	memcpy(take(&p, ZSTR_LEN(key)), ZSTR_VAL(key), ZSTR_LEN(key));
	if (g) {
		ZVAL_PTR(entry_val(e), persist_graph(&p, val));
	} else {
		ZVAL_COPY_VALUE(entry_val(e), val);
		persist_zval(&p, entry_val(e));
	}
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
	zval *v = entry_val(e);
	if (!(e->flags & ENTRY_GRAPH)) {
		rebase_zval(&r, v);
		return;
	}
	nc_graph *g = Z_PTR_P(v) = moved(&r, Z_PTR_P(v));
	nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->ncls; i++) {
		c[i].name = moved(&r, c[i].name);
		c[i].lc = moved(&r, c[i].lc);
		ZSTR_H(c[i].lc) = 0;
	}
	for (uint32_t i = 0; i < g->nobj; i++) {
		nc_obj *o = graph_obj(g, i);
		for (uint32_t k = 0; k < c[o->cls].nprops; k++) {
			rebase_zval(&r, &o->props[k]);
		}
	}
	rebase_zval(&r, &g->root);
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

static void unshare_key(nocopy_keys *keys, zend_array *a, Bucket *b)
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
				unshare_key(keys, a, b);
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

/* ---- leaving shared memory: graphs ---- */

typedef struct {
	nocopy_keys *keys;
	zend_object **objs;
	bool copy; /* copy mode: no arena pointer may escape */
} gout;

static void graph_zval(gout *o, zval *dst, const zval *src);

/* A private copy of a spine: objects and spines below get built, the rest stays shared (zero-copy) or is copied. */
static void graph_contents(gout *o, zend_array *a)
{
	zval t;
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if (o->copy || Z_TYPE_P(v) == NC_OREF || Z_TYPE_P(v) == NC_SPINE) {
				graph_zval(o, &t, v);
				ZVAL_COPY_VALUE(v, &t);
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (o->copy && b->key && in_arena(b->key)) {
				unshare_key(o->keys, a, b);
			}
			if (o->copy || Z_TYPE(b->val) == NC_OREF || Z_TYPE(b->val) == NC_SPINE) {
				graph_zval(o, &t, &b->val);
				ZVAL_COPY_VALUE(&b->val, &t);
			}
		} ZEND_HASH_FOREACH_END();
	}
}

static void graph_zval(gout *o, zval *dst, const zval *src)
{
	switch (Z_TYPE_P(src)) {
		case NC_OREF:
			ZVAL_OBJ_COPY(dst, o->objs[Z_LVAL_P(src)]);
			break;
		case NC_SPINE:
			ZVAL_ARR(dst, zend_array_dup(Z_ARR_P(src)));
			graph_contents(o, Z_ARR_P(dst));
			break;
		default:
			if (o->copy) {
				copy_out(o->keys, dst, src);
			} else {
				ZVAL_COPY_VALUE(dst, src);
			}
	}
}

static zend_class_entry *graph_class(const nc_class *c)
{
	zend_class_entry *ce = zend_hash_find_ptr(EG(class_table), c->lc);
	if (!ce) {
		zend_string *n = zend_string_init(ZSTR_VAL(c->name), ZSTR_LEN(c->name), 0); /* autoloaders may keep it */
		ce = zend_lookup_class(n);
		zend_string_release(n);
	}
	if (!ce || EG(exception) || class_fp(ce) != c->fp || (uint32_t) ce->default_properties_count != c->nprops) {
		return NULL;
	}
	if (!(ce->ce_flags & ZEND_ACC_CONSTANTS_UPDATED) && zend_update_class_constants(ce) != SUCCESS) {
		return NULL;
	}
	return ce;
}

bool nocopy_graph_out(zval *rv, const zval *v, bool copy)
{
	const nc_graph *g = Z_PTR_P(v);
	const nc_class *c = graph_cls(g);
	zend_class_entry *ces_s[8], **ces = g->ncls <= 8 ? ces_s : emalloc(g->ncls * sizeof(*ces));
	bool ok = true;
	for (uint32_t i = 0; i < g->ncls && ok; i++) {
		ok = (ces[i] = graph_class(&c[i])) != NULL; /* all before any object: autoloading may run user code */
	}
	if (ok) {
		zend_object *objs_s[16], **objs = g->nobj <= 16 ? objs_s : emalloc(g->nobj * sizeof(*objs));
		for (uint32_t i = 0; i < g->nobj; i++) {
			objs[i] = zend_objects_new(ces[graph_obj(g, i)->cls]);
		}
		gout o = {.keys = &NOCOPY_G(keys), .objs = objs, .copy = copy};
		if (copy) {
			keys_reset(o.keys);
		}
		for (uint32_t i = 0; i < g->nobj; i++) {
			nc_obj *n = graph_obj(g, i);
			zval *dst = objs[i]->properties_table;
			for (uint32_t k = 0; k < c[n->cls].nprops; k++) {
				graph_zval(&o, &dst[k], &n->props[k]);
				Z_PROP_FLAG_P(&dst[k]) = Z_TYPE(dst[k]) == IS_UNDEF ? (Z_PROP_FLAG_P(&n->props[k]) & IS_PROP_UNINIT) : 0;
			}
		}
		graph_zval(&o, rv, &g->root);
		for (uint32_t i = 0; i < g->nobj; i++) {
			OBJ_RELEASE(objs[i]);
		}
		if (objs != objs_s) {
			efree(objs);
		}
	}
	if (ces != ces_s) {
		efree(ces);
	}
	return ok;
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
	if (Z_TYPE_P(val) == IS_ARRAY || Z_TYPE_P(val) == IS_OBJECT) {
		keys_reset(keys);
	}
	graph_b g;
	g.on = false; /* not = {0}: zeroing its tables cost every store ~3 ns */
	sizing z = {.keys = *keys, .g = &g};
	zval ser;
	bool serialized = false, igb = false;
	size_status st = size_root(&z, val);
	bool graph = st == SIZE_OK && g.on && zend_hash_num_elements(&g.objs);
	if (st != SIZE_OK) {
		graph_b_free(&g);
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
	} else if (graph) {
		z.n += SALIGN(graph_head(zend_hash_num_elements(&g.cls), zend_hash_num_elements(&g.objs)));
	}

	size_t size = SALIGN(sizeof(php_entry)) + SALIGN(ZSTR_LEN(key)) + z.n;
	nocopy_entry *e = !p ? NULL : nocopy_put_begin(p, &NOCOPY_G(ctx), zend_string_hash_val(key), (uint32_t) ZSTR_LEN(key), KEY_OFF, size,
		o->ttl);
	if (e) {
		e->flags = serialized ? ENTRY_SERIALIZED | (igb ? ENTRY_IGBINARY : 0) : graph ? ENTRY_GRAPH : 0;
		persist_entry(e, key, val, keys, graph ? &g : NULL);
	}
	if (graph) {
		graph_b_free(&g);
	}
	if (serialized) {
		zend_string_release(Z_STR(ser));
	}
	return e ? NOCOPY_OK : NOCOPY_NO_ROOM;
}
