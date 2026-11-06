/* nocopy: PHP values in and out of an arena block (size, persist, rebase, copy out, serialize). */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_php.h"
#include "ext/standard/php_var.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "zend_lazy_objects.h"
#include "zend_enum.h"

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

/* ---- object graphs: objects stored as nodes, rebuilt on fetch ---- */

#define NC_OREF IS_OBJECT /* in a graph: Z_LVAL is the object's index */
#define NC_SPINE IS_PTR   /* in a graph: an array with objects below it, copied on fetch */

/* How a class's objects are stored, in the low bits of its fingerprint (0: not as a node, so the value is serialized). */
#define K_SLOTS 1  /* the raw property slots */
#define K_WAKEUP 2 /* the slots, then __wakeup() */
#define K_DATA 3   /* props[0]: __serialize()'s array, given to __unserialize() */
#define K_ENUM 4   /* props[0]: the case's name */
#define K_MASK 7
static zend_always_inline uint32_t fp_kind(uint64_t fp) { return (uint32_t) (fp & K_MASK); }

typedef struct {
	zend_string *name;
	zend_string *lc; /* hashed, for the class table */
	uint64_t fp;
	uint32_t nprops;
} nc_class;

typedef struct {
	uint32_t cls;
	uint32_t dyn; /* 1: props[nprops] is its dynamic properties (an array) */
	zval props[];
} nc_obj;

/* Followed by nc_class[ncls], uint32_t off[nobj] (each object's offset from the graph), uint32_t hooks[nhooks]. */
typedef struct {
	uint32_t ncls, nobj, nhooks;
	zval root;
} nc_graph;

static zend_always_inline nc_class *graph_cls(const nc_graph *g) { return (nc_class *) (g + 1); }
static zend_always_inline uint32_t *graph_off(const nc_graph *g) { return (uint32_t *) (graph_cls(g) + g->ncls); }
static zend_always_inline nc_obj *graph_obj(const nc_graph *g, uint32_t i) { return (nc_obj *) ((char *) g + graph_off(g)[i]); }
/* Objects whose __wakeup/__unserialize runs after the build, inner first (unserialize's order). */
static zend_always_inline uint32_t *graph_hooks(const nc_graph *g) { return graph_off(g) + g->nobj; }
static zend_always_inline size_t graph_head(uint32_t ncls, uint32_t nobj, uint32_t nhooks) { return sizeof(nc_graph) + ncls * sizeof(nc_class) + (nobj + nhooks) * sizeof(uint32_t); }
static zend_always_inline size_t obj_size(uint32_t nprops) { return offsetof(nc_obj, props) + nprops * sizeof(zval); }

static zend_always_inline uint32_t obj_nz(const nc_class *c, const nc_obj *o) { return c[o->cls].nprops + o->dyn; }
static zend_always_inline uint32_t kind_nprops(const zend_class_entry *ce, uint32_t kind) { return kind <= K_WAKEUP ? (uint32_t) ce->default_properties_count : 1; }

/* The kind (as serialize() treats the class) and a hash of what must not change: for slots, their names, visibility, types. */
static uint64_t class_fp_compute(zend_class_entry *ce)
{
	bool data = ce->__serialize || ce->__unserialize;
	if (!(ce->ce_flags & ZEND_ACC_ENUM)
		&& ((ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS
			| ZEND_ACC_ANON_CLASS | ZEND_ACC_NOT_SERIALIZABLE))
		|| (data && (!ce->__serialize || !ce->__unserialize))
		|| (!data && (ce->serialize || ce->unserialize || zend_hash_str_exists(&ce->function_table, ZEND_STRL("__sleep"))
			|| (ce->type != ZEND_USER_CLASS && ce != zend_standard_class_def) || ce->create_object || ce->default_object_handlers != &std_object_handlers)))) {
		return 0;
	}
	uint32_t kind = ce->ce_flags & ZEND_ACC_ENUM ? K_ENUM : data ? K_DATA
		: zend_hash_exists(&ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP)) ? K_WAKEUP : K_SLOTS;
	smart_str b = {0};
	smart_str_append(&b, ce->name);
	smart_str_append_long(&b, kind);
	smart_str_append_long(&b, ce->default_properties_count);
	for (int i = 0; kind <= K_WAKEUP && i < ce->default_properties_count; i++) {
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
	uint64_t fp = (zend_hash_func(ZSTR_VAL(b.s), ZSTR_LEN(b.s)) & ~(uint64_t) K_MASK) | kind;
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
	HashTable objs;  /* handle -> index */
	HashTable cls;   /* class entry -> index << 3 | kind */
	HashTable hooks; /* indices, inner first */
	HashTable data;  /* handle -> __serialize()'s array: called once, kept (with what it made) until the entry is built */
	bool on;
	bool hooks_on;   /* hooks and data set up: only with __wakeup/__serialize classes */
	bool user;       /* __serialize ran: it may have used the key table, so sizing is done again */
} graph_b;

static void graph_b_init(graph_b *g)
{
	zend_hash_init(&g->objs, 8, NULL, NULL, 0);
	zend_hash_init(&g->cls, 4, NULL, NULL, 0);
	g->on = true;
	g->hooks_on = false;
}

static void graph_b_hooks(graph_b *g)
{
	if (!g->hooks_on) {
		zend_hash_init(&g->hooks, 0, NULL, NULL, 0);
		zend_hash_init(&g->data, 0, NULL, ZVAL_PTR_DTOR, 0);
		g->hooks_on = true;
	}
}

static zend_always_inline uint32_t graph_b_nhooks(const graph_b *g) { return g->hooks_on ? zend_hash_num_elements(&g->hooks) : 0; }

static void graph_b_free(graph_b *g)
{
	if (g->on) {
		zend_hash_destroy(&g->objs);
		zend_hash_destroy(&g->cls);
	}
	if (g->hooks_on) {
		zend_hash_destroy(&g->hooks);
		zend_hash_destroy(&g->data);
	}
}

/* ---- sizing ---- */

typedef enum { SIZE_OK, SIZE_DEEP, SIZE_OBJECT, SIZE_RESOURCE, SIZE_ERROR /* exception thrown */ } size_status;

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

/* The dynamic properties: stdClass's table itself, else a copy without the slots (kept in data, by handle). */
static zend_array *dyn_props(graph_b *g, zend_object *o)
{
	if (!o->ce->default_properties_count) {
		return o->properties;
	}
	zval *d = zend_hash_index_find(&g->data, o->handle);
	if (d) {
		return Z_ARR_P(d);
	}
	zval a;
	array_init(&a);
	zend_string *k;
	zend_ulong h;
	zval *v;
	ZEND_HASH_FOREACH_KEY_VAL(o->properties, h, k, v) {
		if (Z_TYPE_P(v) != IS_INDIRECT) {
			Z_TRY_ADDREF_P(v);
			if (k) {
				zend_hash_add_new(Z_ARR(a), k, v);
			} else {
				zend_hash_index_add_new(Z_ARR(a), h, v);
			}
		}
	} ZEND_HASH_FOREACH_END();
	return Z_ARR_P(zend_hash_index_add_new(&g->data, o->handle, &a));
}

/* SIZE_OBJECT: no node for it, so the whole value is serialized. */
__attribute__((noinline)) static size_status size_object(sizing *z, zend_object *o, int depth)
{
	graph_b *g = z->g;
	if (!g->on) {
		graph_b_init(g);
	}
	zval idx;
	ZVAL_LONG(&idx, zend_hash_num_elements(&g->objs));
	if (!zend_hash_index_add(&g->objs, o->handle, &idx)) {
		return SIZE_OK; /* met before: a back reference */
	}
	zend_class_entry *ce = o->ce;
	uint32_t kind = fp_kind(class_fp(ce));
	if (!kind || zend_object_is_lazy(o) || (kind <= K_WAKEUP && o->handlers != &std_object_handlers)) {
		return SIZE_OBJECT;
	}
	zend_long i = Z_LVAL(idx);
	bool dyn = kind <= K_WAKEUP && o->properties && has_dynamic(o->properties);
	if (kind == K_WAKEUP || kind == K_DATA || (dyn && ce->default_properties_count)) {
		graph_b_hooks(g);
	}
	if (!zend_hash_index_exists(&g->cls, (zend_ulong) (uintptr_t) ce)) {
		ZVAL_LONG(&idx, (zend_long) zend_hash_num_elements(&g->cls) << 3 | kind);
		zend_hash_index_add_new(&g->cls, (zend_ulong) (uintptr_t) ce, &idx);
		z->n += 2 * SALIGN(_ZSTR_STRUCT_SIZE(ZSTR_LEN(ce->name)));
	}
	z->n += SALIGN(obj_size(kind_nprops(ce, kind) + dyn));
	size_status st = SIZE_OK;
	if (kind == K_ENUM) {
		z->n += SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN_P(zend_enum_fetch_case_name(o))));
	} else if (kind == K_DATA) {
		zval *d = zend_hash_index_find(&g->data, o->handle), dv;
		if (!d) {
			g->user = true;
			BG(serialize_lock)++;
			zend_call_known_instance_method_with_0_params(ce->__serialize, o, &dv);
			BG(serialize_lock)--;
			if (EG(exception)) {
				zval_ptr_dtor(&dv);
				return SIZE_ERROR;
			}
			if (Z_TYPE(dv) != IS_ARRAY) {
				zval_ptr_dtor(&dv);
				zend_type_error("%s::__serialize() must return an array", ZSTR_VAL(ce->name));
				return SIZE_ERROR;
			}
			d = zend_hash_index_add_new(&g->data, o->handle, &dv);
		}
		ZVAL_COPY_VALUE(&dv, d); /* user code below may grow the table */
		st = size_zval(z, &dv, depth + 1);
	} else {
		for (int k = 0; k < ce->default_properties_count && st == SIZE_OK; k++) {
			st = size_zval(z, OBJ_PROP_NUM(o, k), depth + 1);
		}
		if (dyn && st == SIZE_OK) {
			zval dv;
			ZVAL_ARR(&dv, dyn_props(g, o));
			st = size_zval(z, &dv, depth + 1);
		}
	}
	if (st == SIZE_OK && (kind == K_WAKEUP || kind == K_DATA)) {
		ZVAL_LONG(&idx, i);
		zend_hash_next_index_insert_new(&g->hooks, &idx);
	}
	return st;
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
		zend_long ck = Z_LVAL_P(zend_hash_index_find(&p->g->cls, (zend_ulong) (uintptr_t) ce));
		uint32_t kind = (uint32_t) (ck & K_MASK);
		bool dyn = kind <= K_WAKEUP && o->properties && has_dynamic(o->properties);
		nc_obj *n = take(p, obj_size(kind_nprops(ce, kind) + dyn));
		graph_off(p->graph)[i] = (uint32_t) ((char *) n - (char *) p->graph);
		n->cls = (uint32_t) (ck >> 3);
		n->dyn = dyn;
		if (kind == K_ENUM) {
			ZVAL_STR(&n->props[0], Z_STR_P(zend_enum_fetch_case_name(o)));
			persist_zval(p, &n->props[0]);
		} else if (kind == K_DATA) {
			ZVAL_COPY_VALUE(&n->props[0], zend_hash_index_find(&p->g->data, o->handle));
			persist_zval(p, &n->props[0]);
		} else {
			for (int k = 0; k < ce->default_properties_count; k++) {
				ZVAL_COPY_VALUE(&n->props[k], OBJ_PROP_NUM(o, k));
				persist_zval(p, &n->props[k]);
			}
			if (dyn) {
				zval *d = &n->props[ce->default_properties_count];
				ZVAL_ARR(d, dyn_props(p->g, o));
				persist_zval(p, d);
			}
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
	uint32_t ncls = zend_hash_num_elements(&g->cls), nobj = zend_hash_num_elements(&g->objs), nhooks = graph_b_nhooks(g);
	nc_graph *gr = take(p, graph_head(ncls, nobj, nhooks));
	gr->ncls = ncls;
	gr->nobj = nobj;
	gr->nhooks = nhooks;
	if (nhooks) {
		uint32_t *h = graph_hooks(gr);
		zval *hv;
		ZEND_HASH_FOREACH_VAL(&g->hooks, hv) {
			*h++ = (uint32_t) Z_LVAL_P(hv);
		} ZEND_HASH_FOREACH_END();
	}
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
		c->nprops = kind_nprops(ce, fp_kind(c->fp));
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
		for (uint32_t k = 0; k < obj_nz(c, o); k++) {
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

typedef enum {
	G_ZERO, /* values stay the arena's */
	G_COPY, /* copy mode: no arena pointer may escape */
	G_TPL,  /* from a template: its values are private, shared by refcount */
} gmode;

typedef struct {
	nocopy_keys *keys;
	zend_object **objs;
	gmode mode;
} gout;

static void graph_zval(gout *o, zval *dst, const zval *src);

/* A private copy of a spine: objects and spines below get built, the rest stays shared or is copied. */
static void graph_contents(gout *o, zend_array *a)
{
	bool copy = o->mode == G_COPY;
	zval t;
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if (copy || Z_TYPE_P(v) == NC_OREF || Z_TYPE_P(v) == NC_SPINE) {
				graph_zval(o, &t, v);
				ZVAL_COPY_VALUE(v, &t);
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (copy && b->key && in_arena(b->key)) {
				unshare_key(o->keys, a, b);
			}
			if (copy || Z_TYPE(b->val) == NC_OREF || Z_TYPE(b->val) == NC_SPINE) {
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
			ZVAL_ARR(dst, zend_array_dup(Z_ARR_P(src))); /* a template's spine: dup addrefs its values */
			graph_contents(o, Z_ARR_P(dst));
			break;
		default:
			if (o->mode == G_COPY) {
				copy_out(o->keys, dst, src);
			} else if (o->mode == G_TPL) {
				ZVAL_COPY(dst, src);
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
	if (!ce || EG(exception) || class_fp(ce) != c->fp || kind_nprops(ce, fp_kind(c->fp)) != c->nprops) {
		return NULL;
	}
	if (!(ce->ce_flags & ZEND_ACC_CONSTANTS_UPDATED) && zend_update_class_constants(ce) != SUCCESS) {
		return NULL;
	}
	return ce;
}

/* All classes before any object: autoloading may run user code. */
static bool graph_classes(const nc_graph *g, zend_class_entry **ces)
{
	for (uint32_t i = 0; i < g->ncls; i++) {
		if (!(ces[i] = graph_class(&graph_cls(g)[i]))) {
			return false;
		}
	}
	return true;
}

/* Dynamic properties into a private table (the engine may write a properties table in place). */
static void graph_dyn(gout *o, zend_object *obj, const zval *src)
{
	zval d;
	if (Z_TYPE_P(src) == NC_SPINE || o->mode == G_COPY) {
		graph_zval(o, &d, src);
	} else {
		ZVAL_ARR(&d, zend_array_dup(Z_ARR_P(src)));
	}
	if (!obj->ce->default_properties_count) {
		obj->properties = Z_ARR(d);
		return;
	}
	zend_hash_copy(rebuild_object_properties_internal(obj), Z_ARR(d), zval_add_ref);
	zend_array_release(Z_ARR(d));
}

/* __wakeup/__unserialize, inner first, as unserialize() runs them: after one throws, the rest are marked as never constructed. */
static bool graph_hooks_run(zend_object **objs, zval *data, const uint32_t *hooks, uint32_t n)
{
	bool failed = false;
	BG(serialize_lock)++;
	for (uint32_t k = 0; k < n; k++) {
		zend_object *ob = objs[hooks[k]];
		if (!failed) {
			zval *d = &data[hooks[k]];
			if (Z_TYPE_P(d) != IS_UNDEF) {
				zend_call_known_instance_method_with_1_params(ob->ce->__unserialize, ob, NULL, d);
			} else {
				zval r;
				zend_call_known_instance_method_with_0_params(zend_hash_find_ptr(&ob->ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP)), ob, &r);
				zval_ptr_dtor(&r);
			}
			failed = EG(exception) != NULL;
		}
		if (failed) {
			GC_ADD_FLAGS(ob, IS_OBJ_DESTRUCTOR_CALLED);
		}
	}
	BG(serialize_lock)--;
	return !failed;
}

/* Reads g only before the hooks run user code (which may drop a template). false: an enum case is gone, or a hook threw. */
static bool graph_build(zval *rv, const nc_graph *g, zend_class_entry **ces, gmode mode)
{
	const nc_class *c = graph_cls(g);
	zend_object *objs_s[16], **objs = g->nobj <= 16 ? objs_s : emalloc(g->nobj * sizeof(*objs));
	uint32_t made = 0;
	for (; made < g->nobj; made++) {
		nc_obj *n = graph_obj(g, made);
		zend_class_entry *ce = ces[n->cls];
		uint32_t kind = fp_kind(c[n->cls].fp);
		if (kind == K_ENUM) {
			zend_class_constant *cc = zend_hash_find_ptr(CE_CONSTANTS_TABLE(ce), Z_STR(n->props[0]));
			if (!cc || !(ZEND_CLASS_CONST_FLAGS(cc) & ZEND_CLASS_CONST_IS_CASE)) {
				break;
			}
			objs[made] = zend_enum_get_case(ce, Z_STR(n->props[0]));
			GC_ADDREF(objs[made]);
		} else if (kind == K_DATA) {
			zval t;
			if (object_init_ex(&t, ce) != SUCCESS) {
				break;
			}
			objs[made] = Z_OBJ(t);
		} else {
			objs[made] = zend_objects_new(ce);
		}
	}
	bool ok = made == g->nobj;
	zval *data = NULL;
	if (ok) {
		gout o = {.keys = &NOCOPY_G(keys), .objs = objs, .mode = mode};
		if (mode == G_COPY) {
			keys_reset(o.keys);
		}
		if (g->nhooks) {
			data = ecalloc(g->nobj, sizeof(zval)); /* IS_UNDEF */
		}
		for (uint32_t i = 0; i < g->nobj; i++) {
			nc_obj *n = graph_obj(g, i);
			uint32_t kind = fp_kind(c[n->cls].fp);
			if (kind == K_DATA) {
				graph_zval(&o, &data[i], &n->props[0]);
			} else if (kind <= K_WAKEUP) {
				zval *dst = objs[i]->properties_table;
				for (uint32_t k = 0; k < c[n->cls].nprops; k++) {
					graph_zval(&o, &dst[k], &n->props[k]);
					Z_PROP_FLAG_P(&dst[k]) = Z_TYPE(dst[k]) == IS_UNDEF ? (Z_PROP_FLAG_P(&n->props[k]) & IS_PROP_UNINIT) : 0;
				}
				if (n->dyn) {
					graph_dyn(&o, objs[i], &n->props[c[n->cls].nprops]);
				}
			}
		}
		graph_zval(&o, rv, &g->root);
	} else {
		for (uint32_t i = 0; i < made; i++) {
			if (!(objs[i]->ce->ce_flags & ZEND_ACC_ENUM)) {
				GC_ADD_FLAGS(objs[i], IS_OBJ_DESTRUCTOR_CALLED);
			}
		}
	}
	if (data) {
		uint32_t n = g->nhooks, hs[16], *hooks = n <= 16 ? hs : emalloc(n * sizeof(uint32_t));
		memcpy(hooks, graph_hooks(g), n * sizeof(uint32_t));
		ok = graph_hooks_run(objs, data, hooks, n);
		if (hooks != hs) {
			efree(hooks);
		}
		for (uint32_t i = 0; i < g->nobj; i++) {
			zval_ptr_dtor(&data[i]);
		}
		efree(data);
		if (!ok) {
			zval_ptr_dtor(rv);
			ZVAL_NULL(rv);
		}
	}
	for (uint32_t i = 0; i < made; i++) {
		OBJ_RELEASE(objs[i]);
	}
	if (objs != objs_s) {
		efree(objs);
	}
	return ok;
}

bool nocopy_graph_out(zval *rv, const zval *v, bool copy)
{
	const nc_graph *g = Z_PTR_P(v);
	zend_class_entry *ces_s[8], **ces = g->ncls <= 8 ? ces_s : emalloc(g->ncls * sizeof(*ces));
	bool ok = graph_classes(g, ces) && graph_build(rv, g, ces, copy ? G_COPY : G_ZERO);
	if (ces != ces_s) {
		efree(ces);
	}
	return ok;
}

/* ---- copy mode: a graph's private template, kept in the local table ---- */

/* A private copy of the graph's bytes (offsets stay valid), its values made private and its classes resolved. */
typedef struct {
	zend_class_entry **ces;
	nc_graph g; /* followed by the rest of the block */
} nc_tpl;

/* Private copy of an arena value, keeping graph markers: spines become private arrays. */
static void tpl_zval(nocopy_keys *keys, zval *z)
{
	if (Z_TYPE_P(z) == NC_SPINE) {
		zend_array *a = zend_array_dup(Z_ARR_P(z));
		if (HT_IS_PACKED(a)) {
			zval *v;
			ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
				tpl_zval(keys, v);
			} ZEND_HASH_FOREACH_END();
		} else {
			Bucket *b;
			ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
				if (b->key && in_arena(b->key)) {
					unshare_key(keys, a, b);
				}
				tpl_zval(keys, &b->val);
			} ZEND_HASH_FOREACH_END();
		}
		Z_ARR_P(z) = a;
	} else if (Z_TYPE_P(z) != NC_OREF) {
		unshare_zval(keys, z);
	}
}

static void tpl_free_zval(zval *z)
{
	if (Z_TYPE_P(z) == NC_SPINE) {
		zend_array *a = Z_ARR_P(z);
		zval *v;
		ZEND_HASH_FOREACH_VAL(a, v) {
			if (Z_TYPE_P(v) == NC_SPINE) {
				tpl_free_zval(v); /* the array's destructor skips them: not refcounted */
			}
		} ZEND_HASH_FOREACH_END();
		zend_array_destroy(a);
	} else {
		zval_ptr_dtor(z);
	}
}

void *nocopy_graph_tpl(const nocopy_entry *e)
{
	const nc_graph *g = Z_PTR_P(entry_val((nocopy_entry *) e));
	size_t n = e->size - (size_t) ((const char *) g - (const char *) e);
	nc_tpl *t = emalloc(offsetof(nc_tpl, g) + n + g->ncls * sizeof(zend_class_entry *));
	memcpy(&t->g, g, n);
	t->ces = (zend_class_entry **) ((char *) &t->g + n);
	if (!graph_classes(&t->g, t->ces)) {
		efree(t);
		return NULL;
	}
	nocopy_keys *keys = &NOCOPY_G(keys);
	keys_reset(keys);
	nc_class *c = graph_cls(&t->g);
	for (uint32_t i = 0; i < t->g.nobj; i++) {
		nc_obj *o = graph_obj(&t->g, i);
		for (uint32_t k = 0; k < obj_nz(c, o); k++) {
			tpl_zval(keys, &o->props[k]);
		}
	}
	tpl_zval(keys, &t->g.root);
	return t;
}

void nocopy_graph_tpl_free(void *p)
{
	nc_tpl *t = p;
	nc_class *c = graph_cls(&t->g);
	for (uint32_t i = 0; i < t->g.nobj; i++) {
		nc_obj *o = graph_obj(&t->g, i);
		for (uint32_t k = 0; k < obj_nz(c, o); k++) {
			tpl_free_zval(&o->props[k]);
		}
	}
	tpl_free_zval(&t->g.root);
	efree(t);
}

bool nocopy_graph_tpl_out(zval *rv, void *p)
{
	nc_tpl *t = p;
	return graph_build(rv, &t->g, t->ces, G_TPL);
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
	g.on = g.hooks_on = g.user = false; /* not = {0}: zeroing its tables cost every store ~3 ns */
	sizing z = {.keys = *keys, .g = &g};
	zval ser;
	bool serialized = false, igb = false;
	size_status st = size_root(&z, val);
	if (st == SIZE_OK && g.on && g.user) { /* again without user code: __serialize's arrays are kept */
		keys_reset(keys);
		z.keys = *keys;
		z.n = 0;
		zend_hash_clean(&g.objs);
		zend_hash_clean(&g.cls);
		zend_hash_clean(&g.hooks);
		st = size_root(&z, val);
	}
	bool graph = st == SIZE_OK && g.on && zend_hash_num_elements(&g.objs);
	if (st != SIZE_OK) {
		graph_b_free(&g);
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
		z.n = SALIGN(_ZSTR_STRUCT_SIZE(Z_STRLEN(ser)));
	} else if (graph) {
		z.n += SALIGN(graph_head(zend_hash_num_elements(&g.cls), zend_hash_num_elements(&g.objs), graph_b_nhooks(&g)));
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
