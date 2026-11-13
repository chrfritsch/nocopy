/* nocopy: values out of the arena. Private copies, graphs rebuilt as objects, copy mode's templates. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "ext/standard/basic_functions.h"
#include "zend_enum.h"

/* ---- private copies ---- */

static void copy_into(nocopy_strtab *t, zval *dst, const zval *src);

/* A string the block uses more than once (refcount 3): its uses share one copy. */
static __attribute__((noinline)) void copy_shared_str(nocopy_strtab *t, zval *dst, zend_string *s)
{
	nocopy_strslot *k = strtab_slot(t, ZSTR_H(s));
	if (k->gen == t->gen && k->s == s) {
		ZVAL_STR_COPY(dst, k->copy);
		return;
	}
	strtab_put(t, k, s, ZSTR_H(s));
	k->copy = zend_string_init(ZSTR_VAL(s), ZSTR_LEN(s), 0);
	ZSTR_H(k->copy) = ZSTR_H(s);
	ZVAL_STR(dst, k->copy);
}

/* A key still in the arena: a deduped one shares its copy. */
static void unshare_key(nocopy_strtab *t, zend_array *a, Bucket *b)
{
	nocopy_strslot *s = strtab_slot(t, b->h);
	if (s->gen == t->gen && s->s == b->key) {
		b->key = zend_string_copy(s->copy);
	} else {
		strtab_put(t, s, b->key, b->h);
		b->key = s->copy = zend_string_init(ZSTR_VAL(b->key), ZSTR_LEN(b->key), 0);
		ZSTR_H(b->key) = b->h;
	}
	HT_FLAGS(a) &= ~HASH_FLAG_STATIC_KEYS;
}

/* An arena string or array inside a private array: replaced by a private copy. */
static void unshare_zval(void *t, zval *z)
{
	if ((Z_TYPE_P(z) == IS_STRING || Z_TYPE_P(z) == IS_ARRAY) && nocopy_in_arena(Z_COUNTED_P(z))) {
		zval c;
		copy_into(t, &c, z);
		ZVAL_COPY_VALUE(z, &c);
	}
}

/* Each value of a private copy of an arena array through f; with t, the keys still in the arena get private copies first. */
static zend_always_inline void array_each(zend_array *a, nocopy_strtab *t, zval_fn f, void *ctx)
{
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			f(ctx, v);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (t && b->key && nocopy_in_arena(b->key)) {
				unshare_key(t, a, b);
			}
			f(ctx, &b->val);
		} ZEND_HASH_FOREACH_END();
	}
}

static void copy_into(nocopy_strtab *t, zval *dst, const zval *src)
{
	switch (Z_TYPE_P(src)) {
		case IS_STRING:
			if (UNEXPECTED(GC_REFCOUNT(Z_STR_P(src)) > 2)) {
				copy_shared_str(t, dst, Z_STR_P(src));
			} else {
				ZVAL_STRINGL_FAST(dst, Z_STRVAL_P(src), Z_STRLEN_P(src)); /* "" and 1 char: interned, no alloc */
			}
			break;
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(src);
			if (zend_hash_num_elements(a) == 0) {
				ZVAL_EMPTY_ARRAY(dst);
				break;
			}
			/* bulk-copy the buckets, then repoint whatever still lives in the arena (stored values are at most NC_MAX_DEPTH deep) */
			ZVAL_ARR(dst, zend_array_dup(a));
			array_each(Z_ARR_P(dst), t, unshare_zval, t);
			break;
		}
		default:
			ZVAL_COPY_VALUE(dst, src);
	}
}

void nocopy_value_copy(nocopy_env *env, zval *dst, const zval *src)
{
	if (Z_TYPE_P(src) == IS_ARRAY) {
		strtab_reset(&env->strs);
	}
	copy_into(&env->strs, dst, src);
}

/* ---- graphs ---- */

typedef enum {
	G_ZERO, /* values stay the arena's */
	G_COPY, /* copy mode: no arena pointer may escape */
	G_TPL,  /* from a template: its values are private, shared by refcount */
} gmode;

typedef struct {
	nocopy_env *env;
	zend_object **objs;
	zend_reference **refs;
	gmode mode;
} gout;

static void graph_zval_slow(gout *o, zval *dst, const zval *src);

/* No marker, not copy mode: shared without a call (arena values carry no refcount flag). */
static zend_always_inline void graph_zval(gout *o, zval *dst, const zval *src)
{
	if (EXPECTED(o->mode != G_COPY && Z_TYPE_P(src) <= IS_ARRAY)) {
		ZVAL_COPY(dst, src);
	} else {
		graph_zval_slow(o, dst, src);
	}
}

static void graph_elem(void *ctx, zval *v)
{
	gout *o = ctx;
	if (o->mode == G_COPY || Z_TYPE_P(v) > IS_ARRAY) {
		zval t;
		graph_zval(o, &t, v);
		ZVAL_COPY_VALUE(v, &t);
	}
}

static __attribute__((noinline)) void graph_zval_slow(gout *o, zval *dst, const zval *src)
{
	switch (Z_TYPE_P(src)) {
		case NC_OREF:
			ZVAL_OBJ_COPY(dst, o->objs[Z_LVAL_P(src)]);
			break;
		case NC_SPINE: /* a private copy: objects and spines below get built, the rest stays shared or is copied */
			ZVAL_ARR(dst, zend_array_dup(Z_ARR_P(src))); /* a template's spine: dup addrefs its values */
			array_each(Z_ARR_P(dst), o->mode == G_COPY ? &o->env->strs : NULL, graph_elem, o);
			break;
		case NC_RREF:
			GC_ADDREF(o->refs[Z_LVAL_P(src)]);
			ZVAL_REF(dst, o->refs[Z_LVAL_P(src)]);
			break;
		default:
			if (o->mode == G_COPY) {
				copy_into(&o->env->strs, dst, src);
			} else if (o->mode == G_TPL) {
				ZVAL_COPY(dst, src);
			} else {
				ZVAL_COPY_VALUE(dst, src);
			}
	}
}

/* All classes before any object: autoloading may run user code. */
static bool graph_classes(nocopy_env *env, const nc_graph *g, zend_class_entry **ces)
{
	for (uint32_t i = 0; i < g->ncls; i++) {
		if (!(ces[i] = nc_class_resolve(env, &graph_cls(g)[i]))) {
			return false;
		}
	}
	return true;
}

/* An object, empty (no constructor; an enum case looked up; a date whole). NULL: the case or the date's zone is gone. */
static zend_object *object_make(nocopy_env *env, zend_class_entry *ce, nc_kind kind, const nc_obj *n)
{
	switch (kind) {
		case K_ENUM: {
			zend_class_constant *cc = zend_hash_find_ptr(CE_CONSTANTS_TABLE(ce), Z_STR(n->props[0]));
			if (!cc || !(ZEND_CLASS_CONST_FLAGS(cc) & ZEND_CLASS_CONST_IS_CASE)) {
				return NULL;
			}
			zend_object *o = Z_TYPE(cc->value) == IS_OBJECT ? Z_OBJ(cc->value) : zend_enum_get_case(ce, Z_STR(n->props[0]));
			GC_ADDREF(o);
			return o;
		}
		case K_DATA: case K_DATE: {
			zval t;
			if (object_init_ex(&t, ce) != SUCCESS) {
				return NULL;
			}
			if (kind == K_DATE && !nc_date_out(env, Z_OBJ(t), n)) {
				zval_ptr_dtor(&t);
				return NULL;
			}
			return Z_OBJ(t);
		}
		case K_SLOTS: case K_WAKEUP: case K_SLEEP: case K_SLEEPW:
			return zend_objects_new(ce);
		case K_NONE:
			break;
	}
	return NULL;
}

/* Every object, empty. Returns how many were made: fewer if one could not be. */
static uint32_t graph_objects_new(nocopy_env *env, const nc_graph *g, zend_class_entry **ces, zend_object **objs)
{
	const nc_class *c = graph_cls(g);
	uint32_t i = 0;
	for (; i < g->nobj; i++) {
		const nc_obj *n = graph_obj(g, i);
		if (!(objs[i] = object_make(env, ces[n->cls], fp_kind(c[n->cls].fp), n))) {
			break;
		}
	}
	return i;
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

/* The slots, NC_DEFAULT ones (left out by __sleep()) from the class defaults as unserialize() leaves them; then the dynamic properties. */
static void graph_slots(gout *o, zend_object *obj, const nc_obj *n, uint32_t np)
{
	zval *dst = obj->properties_table, *def = CE_DEFAULT_PROPERTIES_TABLE(obj->ce); /* evaluated: the raw table of an immutable (opcache) class keeps constant ASTs */
	for (uint32_t k = 0; k < np; k++) {
		if (UNEXPECTED(Z_TYPE(n->props[k]) == NC_DEFAULT || Z_TYPE(n->props[k]) == IS_UNDEF)) {
			ZVAL_COPY_OR_DUP_PROP(&dst[k], &def[k]); /* unset too: unserialize() brings back the default */
		} else {
			graph_zval(o, &dst[k], &n->props[k]);
			Z_PROP_FLAG_P(&dst[k]) = 0;
		}
	}
	if (n->dyn) {
		graph_dyn(o, obj, &n->props[np]);
	}
}

/* A typed slot holding a shared reference becomes one of its type sources. */
static __attribute__((noinline)) void graph_ref_types(const nc_graph *g, zend_object **objs)
{
	const nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->nobj; i++) {
		const nc_obj *n = graph_obj(g, i);
		if (!kind_slots(fp_kind(c[n->cls].fp))) {
			continue;
		}
		for (uint32_t k = 0; k < c[n->cls].nprops; k++) {
			zend_property_info *pi = objs[i]->ce->properties_info_table[k];
			if (Z_TYPE(n->props[k]) == NC_RREF && ZEND_TYPE_IS_SET(pi->type)) {
				ZEND_REF_ADD_TYPE_SOURCE(Z_REF(objs[i]->properties_table[k]), pi);
			}
		}
	}
}

/* Its nested arrays private as well: an internal class may write the array __unserialize() got in place
 * (ArrayObject iterated by reference), and a template's arrays are shared with the next fetch. */
static void own_arrays(zval *z, int depth)
{
	if (Z_TYPE_P(z) != IS_ARRAY || depth > NC_MAX_DEPTH) {
		return;
	}
	SEPARATE_ARRAY(z);
	zval *v;
	ZEND_HASH_FOREACH_VAL(Z_ARR_P(z), v) {
		own_arrays(v, depth + 1);
	} ZEND_HASH_FOREACH_END();
}

/* The shared references' values, the objects' contents (K_DATA: data[i], for __unserialize()), the root. */
static void graph_fill(gout *o, const nc_graph *g, zval *data, zval *rv)
{
	const nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->nref; i++) {
		graph_zval(o, &o->refs[i]->val, &graph_refs(g)[i]);
	}
	for (uint32_t i = 0; i < g->nobj; i++) {
		const nc_obj *n = graph_obj(g, i);
		nc_kind kind = fp_kind(c[n->cls].fp);
		switch (kind) {
			case K_DATA:
				graph_zval(o, &data[i], &n->props[0]);
				if (o->mode == G_TPL && o->objs[i]->ce->type == ZEND_INTERNAL_CLASS) {
					own_arrays(&data[i], 0);
				}
				break;
			case K_SLOTS: case K_WAKEUP: case K_SLEEP: case K_SLEEPW:
				graph_slots(o, o->objs[i], n, c[n->cls].nprops);
				break;
			case K_ENUM: case K_DATE: case K_NONE:
				break;
		}
	}
	if (g->nref) {
		graph_ref_types(g, o->objs);
	}
	graph_zval(o, rv, &g->root);
}

/* __wakeup/__unserialize, inner first, as unserialize() runs them: after one throws, the rest are marked as never constructed.
 * Over a copy of the list: user code may drop a template, and g with it. */
static bool graph_hooks_run(const nc_graph *g, zend_object **objs, zval *data)
{
	uint32_t n = g->nhooks, hs[16], *hooks = SMALL_ALLOC(hs, n);
	memcpy(hooks, graph_hooks(g), n * sizeof(uint32_t));
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
	SMALL_FREE(hs, hooks);
	return !failed;
}

/* Reads g only before the hooks run user code. false: an object could not be made, or a hook threw. */
static bool graph_build(nocopy_env *env, zval *rv, const nc_graph *g, zend_class_entry **ces, gmode mode)
{
	zend_object *objs_s[16], **objs = SMALL_ALLOC(objs_s, g->nobj);
	zend_reference *refs_s[8], **refs = SMALL_ALLOC(refs_s, g->nref);
	uint32_t made = graph_objects_new(env, g, ces, objs), nref = 0;
	bool ok = made == g->nobj;
	if (ok) {
		gout o = {.env = env, .objs = objs, .refs = refs, .mode = mode};
		if (mode == G_COPY) {
			strtab_reset(&env->strs);
		}
		for (; nref < g->nref; nref++) {
			zval t;
			ZVAL_NEW_EMPTY_REF(&t);
			ZVAL_NULL(Z_REFVAL(t));
			refs[nref] = Z_REF(t);
		}
		zval *data = g->nhooks ? ecalloc(g->nobj, sizeof(zval)) : NULL; /* IS_UNDEF */
		graph_fill(&o, g, data, rv);
		if (data) {
			ok = graph_hooks_run(g, objs, data);
			for (uint32_t i = 0; i < made; i++) {
				zval_ptr_dtor(&data[i]);
			}
			efree(data);
			if (!ok) {
				zval_ptr_dtor(rv);
				ZVAL_NULL(rv);
			}
		}
	} else {
		const nc_class *c = graph_cls(g);
		for (uint32_t i = 0; i < made; i++) { /* never constructed: no destructor */
			if (!(objs[i]->ce->ce_flags & ZEND_ACC_ENUM)) {
				GC_ADD_FLAGS(objs[i], IS_OBJ_DESTRUCTOR_CALLED);
			}
			if (kind_slots(fp_kind(c[graph_obj(g, i)->cls].fp))) { /* zend_objects_new left its slots unset: freeing reads them */
				for (int k = 0; k < objs[i]->ce->default_properties_count; k++) {
					ZVAL_UNDEF(&objs[i]->properties_table[k]);
				}
			}
		}
	}
	for (uint32_t i = 0; i < nref; i++) {
		zval t;
		ZVAL_REF(&t, refs[i]);
		zval_ptr_dtor(&t);
	}
	SMALL_FREE(refs_s, refs);
	for (uint32_t i = 0; i < made; i++) {
		OBJ_RELEASE(objs[i]);
	}
	SMALL_FREE(objs_s, objs);
	return ok;
}

bool nc_graph_out(nocopy_env *env, zval *rv, const zval *v, bool copy)
{
	const nc_graph *g = Z_PTR_P(v);
	zend_class_entry *ces_s[8], **ces = SMALL_ALLOC(ces_s, g->ncls);
	bool ok = graph_classes(env, g, ces) && graph_build(env, rv, g, ces, copy ? G_COPY : G_ZERO);
	SMALL_FREE(ces_s, ces);
	return ok;
}

/* ---- copy mode's templates: a graph's bytes with private values and resolved classes ---- */

struct nocopy_tpl {
	zend_class_entry **ces;
	nc_graph *g; /* in bytes: objects lie before it */
	char bytes[]; /* the block's value: everything after the key */
};

/* Private copy of an arena value, keeping graph markers: spines become private arrays. */
static void tpl_zval(void *t, zval *z)
{
	if (Z_TYPE_P(z) == NC_SPINE) {
		zend_array *a = zend_array_dup(Z_ARR_P(z));
		array_each(a, t, tpl_zval, t);
		Z_ARR_P(z) = a;
	} else if (Z_TYPE_P(z) <= IS_ARRAY) {
		unshare_zval(t, z);
	}
}

static void tpl_free_zval(void *ctx, zval *z)
{
	(void) ctx;
	if (Z_TYPE_P(z) == NC_SPINE) {
		zend_array *a = Z_ARR_P(z);
		zval *v;
		ZEND_HASH_FOREACH_VAL(a, v) {
			if (Z_TYPE_P(v) == NC_SPINE) {
				tpl_free_zval(NULL, v); /* the array's destructor skips them: not refcounted */
			}
		} ZEND_HASH_FOREACH_END();
		zend_array_destroy(a);
	} else {
		zval_ptr_dtor(z);
	}
}

static zend_always_inline size_t g_ncls_size(const nocopy_entry *e)
{
	return ((const nc_graph *) Z_PTR_P(nocopy_value_zval(e)))->ncls * sizeof(zend_class_entry *);
}

nocopy_tpl *nocopy_tpl_make(nocopy_env *env, const nocopy_entry *e)
{
	const char *v = (const char *) e + KEY_OFF + SALIGN(nocopy_entry_klen(e));
	size_t n = SALIGN(nocopy_entry_size(e) - (size_t) (v - (const char *) e));
	nocopy_tpl *t = emalloc(offsetof(nocopy_tpl, bytes) + n + g_ncls_size(e));
	memcpy(t->bytes, v, n);
	t->g = (nc_graph *) (t->bytes + ((const char *) Z_PTR_P(nocopy_value_zval(e)) - v));
	t->ces = (zend_class_entry **) (t->bytes + n);
	if (!graph_classes(env, t->g, t->ces)) {
		efree(t);
		return NULL;
	}
	strtab_reset(&env->strs);
	graph_each(t->g, tpl_zval, &env->strs);
	return t;
}

void nocopy_tpl_free(nocopy_tpl *t)
{
	graph_each(t->g, tpl_free_zval, NULL);
	efree(t);
}

bool nocopy_tpl_read(nocopy_env *env, nocopy_tpl *t, zval *rv)
{
	return graph_build(env, rv, t->g, t->ces, G_TPL);
}
