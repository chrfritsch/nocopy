/* nocopy: object graphs. Objects and shared references stored as nodes of an nc_graph in the block, rebuilt on fetch. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "ext/standard/basic_functions.h"
#include "zend_smart_str.h"
#include "zend_lazy_objects.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "ext/date/php_date.h"

/* ---- the layout in the block ---- */

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

/* Followed by nc_class[ncls], zval refs[nref] (shared references' values), uint32_t off[nobj] (each object's offset from the graph), uint32_t hooks[nhooks]. */
struct nc_graph {
	uint32_t ncls, nobj, nhooks, nref;
	zval root;
};

static zend_always_inline nc_class *graph_cls(const nc_graph *g) { return (nc_class *) (g + 1); }
static zend_always_inline zval *graph_refs(const nc_graph *g) { return (zval *) (graph_cls(g) + g->ncls); }
static zend_always_inline uint32_t *graph_off(const nc_graph *g) { return (uint32_t *) (graph_refs(g) + g->nref); }
static zend_always_inline nc_obj *graph_obj(const nc_graph *g, uint32_t i) { return (nc_obj *) ((char *) g + graph_off(g)[i]); }
/* Objects whose __wakeup/__unserialize runs after the build, inner first (unserialize's order). */
static zend_always_inline uint32_t *graph_hooks(const nc_graph *g) { return graph_off(g) + g->nobj; }
static zend_always_inline size_t graph_head(uint32_t ncls, uint32_t nref, uint32_t nobj, uint32_t nhooks) { return sizeof(nc_graph) + ncls * sizeof(nc_class) + nref * sizeof(zval) + (nobj + nhooks) * sizeof(uint32_t); }
static zend_always_inline size_t obj_size(uint32_t nprops) { return offsetof(nc_obj, props) + nprops * sizeof(zval); }

static zend_always_inline uint32_t obj_nz(const nc_class *c, const nc_obj *o) { return c[o->cls].nprops + o->dyn; }

/* Every value a graph holds: its objects' slots, its shared references' values, the root. */
static zend_always_inline void graph_each(nc_graph *g, zval_fn f, void *ctx)
{
	nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->nobj; i++) {
		nc_obj *o = graph_obj(g, i);
		for (uint32_t k = 0; k < obj_nz(c, o); k++) {
			f(ctx, &o->props[k]);
		}
	}
	for (uint32_t i = 0; i < g->nref; i++) {
		f(ctx, &graph_refs(g)[i]);
	}
	f(ctx, &g->root);
}

/* ---- classes: how their objects are stored, and what must not change ---- */

/* The kind, in the low bits of a class's fingerprint (0: not as a node, so the value is serialized). */
#define K_SLOTS 1  /* the raw property slots */
#define K_WAKEUP 2 /* the slots, then __wakeup() */
#define K_SLEEP 3  /* the slots __sleep() names, NC_DEFAULT in the others */
#define K_SLEEPW 4 /* K_SLEEP, then __wakeup() */
#define K_DATA 5   /* props[0]: __serialize()'s array, given to __unserialize() */
#define K_ENUM 6   /* props[0]: the case's name */
#define K_DATE 7   /* DateTime(Immutable): props[0] timestamp, [1] date_word(), [2] zone ID or abbreviation */
#define K_MASK 7
static zend_always_inline uint32_t fp_kind(uint64_t fp) { return (uint32_t) (fp & K_MASK); }
static zend_always_inline bool kind_slots(uint32_t kind) { return kind <= K_SLEEPW; }
static zend_always_inline bool kind_sleep(uint32_t kind) { return kind == K_SLEEP || kind == K_SLEEPW; }
static zend_always_inline bool kind_hook(uint32_t kind) { return kind == K_WAKEUP || kind == K_SLEEPW || kind == K_DATA; }
static zend_always_inline uint32_t kind_nprops(const zend_class_entry *ce, uint32_t kind) { return kind_slots(kind) ? (uint32_t) ce->default_properties_count : kind == K_DATE ? 3 : 1; }

/* The kind (as serialize() treats the class) and a hash of what must not change: for slots, their names, visibility, types. */
static uint64_t class_fp_compute(zend_class_entry *ce)
{
	bool data = ce->__serialize || ce->__unserialize;
	if (!(ce->ce_flags & ZEND_ACC_ENUM)
		&& ((ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS
			| ZEND_ACC_ANON_CLASS | ZEND_ACC_NOT_SERIALIZABLE))
		|| (data && (!ce->__serialize || !ce->__unserialize))
		|| (!data && (ce->serialize || ce->unserialize
			|| (ce->type != ZEND_USER_CLASS && ce != zend_standard_class_def) || ce->create_object || ce->default_object_handlers != &std_object_handlers)))) {
		return 0;
	}
	bool wake = zend_hash_exists(&ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP));
	uint32_t kind = ce->ce_flags & ZEND_ACC_ENUM ? K_ENUM : ce == php_date_get_date_ce() || ce == php_date_get_immutable_ce() ? K_DATE : data ? K_DATA
		: zend_hash_str_exists(&ce->function_table, ZEND_STRL("__sleep")) ? (wake ? K_SLEEPW : K_SLEEP) : wake ? K_WAKEUP : K_SLOTS;
	smart_str b = {0};
	smart_str_append(&b, ce->name);
	smart_str_append_long(&b, kind);
	smart_str_append_long(&b, ce->default_properties_count);
	for (int i = 0; kind_slots(kind) && i < ce->default_properties_count; i++) {
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

/* Cached per request by class entry. */
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
	if (NOCOPY_G(zones_on)) {
		zend_hash_destroy(&NOCOPY_G(zones));
		NOCOPY_G(zones_on) = false;
	}
}

static bool has_dynamic(const HashTable *props);

/* ---- dates: stored as timestamp and zone, not __serialize()'s string (formatting and parsing were ~95% of their cost) ---- */

/* Stored natively: initialized, with a zone, no dynamic properties (__serialize() would add them). */
static bool date_native(zend_object *o)
{
	const timelib_time *t = php_date_obj_from_obj(o)->time;
	return t && t->is_localtime && t->zone_type >= TIMELIB_ZONETYPE_OFFSET && t->zone_type <= TIMELIB_ZONETYPE_ID
		&& !(o->properties && has_dynamic(o->properties));
}

/* microseconds | zone type << 20 | dst << 24 | UTC offset << 32 */
static uint64_t date_word(const timelib_time *t)
{
	return (uint64_t) t->us | (uint64_t) t->zone_type << 20 | (uint64_t) (t->dst != 0) << 24 | (uint64_t) (uint32_t) t->z << 32;
}

/* ID or abbreviation, as a string (persisted like a slot's), else null. */
static bool date_zone_name(zend_object *o, zval *z)
{
	const timelib_time *t = php_date_obj_from_obj(o)->time;
	const char *s = t->zone_type == TIMELIB_ZONETYPE_ID ? t->tz_info->name : t->zone_type == TIMELIB_ZONETYPE_ABBR ? t->tz_abbr : NULL;
	if (!s) {
		ZVAL_NULL(z);
		return false;
	}
	ZVAL_STRING(z, s);
	return true;
}

/* The zone as ext/date resolves it (its tzdb, its cache), cached by name per request. NULL: not in the tzdb anymore. */
static timelib_tzinfo *date_tz(const zend_string *name)
{
	HashTable *t = &NOCOPY_G(zones);
	if (!NOCOPY_G(zones_on)) {
		zend_hash_init(t, 8, NULL, NULL, 0);
		NOCOPY_G(zones_on) = true;
	}
	timelib_tzinfo *tz = zend_hash_str_find_ptr(t, ZSTR_VAL(name), ZSTR_LEN(name));
	if (tz) {
		return tz;
	}
	zval o, n;
	object_init_ex(&o, php_date_get_timezone_ce());
	ZVAL_STRINGL(&n, ZSTR_VAL(name), ZSTR_LEN(name));
	zend_call_known_instance_method_with_1_params(Z_OBJCE(o)->constructor, Z_OBJ(o), NULL, &n);
	zval_ptr_dtor(&n);
	if (EG(exception)) {
		zend_clear_exception();
	} else if (Z_PHPTIMEZONE_P(&o)->type == TIMELIB_ZONETYPE_ID) {
		tz = Z_PHPTIMEZONE_P(&o)->tzi.tz;
		zend_hash_str_add_new_ptr(t, ZSTR_VAL(name), ZSTR_LEN(name), tz);
	}
	zval_ptr_dtor(&o);
	return tz;
}

/* Built by ext/date's own calls (timelib is not exported): UTC from the timestamp, then date_timezone_set() with the stored zone. false: the zone is gone. */
static bool date_out(zend_object *obj, const nc_obj *n)
{
	static zend_function *tz_set;
	uint64_t w = (uint64_t) Z_LVAL(n->props[1]);
	int type = (int) (w >> 20) & 0xf;
	int32_t z = (int32_t) (w >> 32);
	zend_object *d = obj; /* date_timezone_set() takes only a DateTime: an immutable one is built in one and takes its time */
	if (obj->ce != php_date_get_date_ce()) {
		zval t;
		object_init_ex(&t, php_date_get_date_ce());
		d = Z_OBJ(t);
	}
	php_date_initialize_from_ts_long(php_date_obj_from_obj(d), Z_LVAL(n->props[0]), (int) (w & 0xfffff));
	bool ok = true;
	if (type != TIMELIB_ZONETYPE_OFFSET || z) {
		zval args[2], rv;
		object_init_ex(&args[1], php_date_get_timezone_ce());
		php_timezone_obj *tz = Z_PHPTIMEZONE_P(&args[1]);
		if (type == TIMELIB_ZONETYPE_ID) {
			ok = (tz->tzi.tz = date_tz(Z_STR(n->props[2]))) != NULL;
		} else if (type == TIMELIB_ZONETYPE_ABBR) {
			tz->tzi.z.utc_offset = z;
			tz->tzi.z.dst = (int) (w >> 24) & 1;
			tz->tzi.z.abbr = timelib_strdup(Z_STRVAL(n->props[2]));
		} else {
			tz->tzi.utc_offset = z;
		}
		tz->type = type;
		tz->initialized = ok;
		if (ok) {
			if (!tz_set) {
				tz_set = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("date_timezone_set"));
			}
			ZVAL_OBJ(&args[0], d);
			zend_call_known_function(tz_set, NULL, NULL, &rv, 2, args, NULL);
			zval_ptr_dtor(&rv);
			ok = !EG(exception);
		}
		zval_ptr_dtor(&args[1]);
	}
	if (d != obj) {
		php_date_obj_from_obj(obj)->time = php_date_obj_from_obj(d)->time;
		php_date_obj_from_obj(d)->time = NULL;
		OBJ_RELEASE(d);
	}
	return ok;
}

/* A magic method with no arguments, run as serialize() runs it. false: it threw (rv is released). */
static bool call_magic(zend_function *f, zend_object *o, zval *rv)
{
	BG(serialize_lock)++;
	zend_call_known_instance_method_with_0_params(f, o, rv);
	BG(serialize_lock)--;
	if (EG(exception)) {
		zval_ptr_dtor(rv);
		return false;
	}
	return true;
}

/* ---- pointer maps ---- */

static void pmap_init(pmap *m)
{
	memset(m->inl, 0, sizeof(m->inl));
	m->s = m->inl;
	m->mask = 31;
	m->n = 0;
}

static void pmap_free(pmap *m)
{
	if (m->s != m->inl) {
		efree(m->s);
	}
}

static zend_always_inline uint32_t pmap_hash(uintptr_t k) { return (uint32_t) __builtin_bswap64((uint64_t) k * 0x5e2d58d8b3bce8d9ULL); }

static zend_always_inline pslot *pmap_probe(const pmap *m, uintptr_t k)
{
	uint32_t i = pmap_hash(k) & m->mask;
	while (m->s[i].k && m->s[i].k != k) {
		i = (i + 1) & m->mask;
	}
	return &m->s[i];
}

static void pmap_grow(pmap *m)
{
	pslot *old = m->s;
	uint32_t n = m->mask + 1;
	m->s = ecalloc(2 * n, sizeof(pslot));
	m->mask = 2 * n - 1;
	for (uint32_t i = 0; i < n; i++) {
		if (old[i].k) {
			*pmap_probe(m, old[i].k) = old[i];
		}
	}
	if (old != m->inl) {
		efree(old);
	}
}

/* k's slot, added with value v if it was missing; *added says which. */
static zend_always_inline pslot *pmap_add(pmap *m, uintptr_t k, uint32_t v, bool *added)
{
	if (2 * (m->n + 1) > m->mask + 1) {
		pmap_grow(m);
	}
	pslot *s = pmap_probe(m, k);
	if ((*added = !s->k)) {
		s->k = k;
		s->v = v;
		m->n++;
	}
	return s;
}

/* ---- the builder: what sizing learns, for persisting ---- */

#define REF_DONE 0x80000000u

static void graph_b_init_maps(graph_b *g)
{
	pmap_init(&g->objs);
	pmap_init(&g->cls);
	g->refs_on = false;
	g->nref = 0;
}

static void graph_b_init(graph_b *g)
{
	graph_b_init_maps(g);
	g->nodes = g->inl;
	g->ncap = 16;
	g->nn = g->nh = g->hcap = 0;
	g->hooks = NULL;
	g->nres = g->nrun = 0;
	g->rcap = GHOOK_INL;
	g->res = g->rinl;
	g->hk = NULL;
	g->on = true;
}

static gnode *graph_b_node(graph_b *g)
{
	if (g->nn == g->ncap) {
		g->ncap *= 4;
		g->nodes = g->nodes == g->inl ? memcpy(emalloc(g->ncap * sizeof(gnode)), g->inl, sizeof(g->inl)) : erealloc(g->nodes, g->ncap * sizeof(gnode));
	}
	gnode *n = &g->nodes[g->nn++];
	ZVAL_UNDEF(&n->data);
	n->named = NULL;
	n->dyn = n->done = n->own = false;
	return n;
}

static void graph_b_hook(graph_b *g, uint32_t i)
{
	if (g->nh == g->hcap) {
		g->hcap = g->hcap ? 2 * g->hcap : 16;
		g->hooks = erealloc(g->hooks, g->hcap * sizeof(uint32_t));
	}
	g->hooks[g->nh++] = i;
}

static void graph_b_maps_free(graph_b *g)
{
	pmap_free(&g->objs);
	pmap_free(&g->cls);
	if (g->refs_on) {
		pmap_free(&g->refs);
	}
}

static void graph_b_nodes_free(graph_b *g)
{
	for (uint32_t i = 0; i < g->nn; i++) {
		if (g->nodes[i].own) {
			zval_ptr_dtor(&g->nodes[i].data);
		}
	}
	g->nn = 0;
}

void nocopy_graph_b_rewind(graph_b *g)
{
	graph_b_maps_free(g);
	graph_b_init_maps(g);
	graph_b_nodes_free(g);
	g->nh = 0;
}

void nocopy_graph_b_free(graph_b *g)
{
	if (!g->on) {
		return;
	}
	graph_b_maps_free(g);
	graph_b_nodes_free(g);
	if (g->nodes != g->inl) {
		efree(g->nodes);
	}
	if (g->hooks) {
		efree(g->hooks);
	}
	for (uint32_t i = 0; i < g->nres; i++) { /* may run destructors: never under a shard lock */
		ghook *h = &g->res[i];
		zval_ptr_dtor(&h->data);
		if (h->named) {
			efree(h->named);
		}
		OBJ_RELEASE(h->o);
	}
	if (g->res != g->rinl) {
		efree(g->res);
	}
	if (g->hk) {
		pmap_free(g->hk);
		efree(g->hk);
	}
}

/* o's hook result, recorded (not run) the first time. */
static ghook *graph_b_res(graph_b *g, zend_object *o)
{
	uint32_t i;
	bool added = true;
	if (!g->hk) {
		for (i = 0; i < g->nres; i++) {
			if (g->res[i].o == o) {
				return &g->res[i];
			}
		}
		if (g->nres == GHOOK_INL) {
			pmap_init(g->hk = emalloc(sizeof(pmap)));
			for (uint32_t k = 0; k < g->nres; k++) {
				pmap_add(g->hk, g->res[k].o->handle, k, &added);
			}
		}
	}
	if (g->hk) {
		i = pmap_add(g->hk, o->handle, g->nres, &added)->v;
	}
	if (added) {
		if (g->nres == g->rcap) {
			g->rcap *= 4;
			g->res = g->res == g->rinl ? memcpy(emalloc(g->rcap * sizeof(ghook)), g->rinl, sizeof(g->rinl)) : erealloc(g->res, g->rcap * sizeof(ghook));
		}
		i = g->nres;
		ghook *h = &g->res[g->nres++];
		GC_ADDREF(o);
		h->o = o;
		ZVAL_UNDEF(&h->data);
		h->named = NULL;
		h->ran = false;
	}
	return &g->res[i];
}

size_t nocopy_graph_head(const graph_b *g)
{
	return SALIGN(graph_head(g->cls.n, g->nref, g->objs.n, g->nh));
}

/* The dynamic properties stored after the slots (NULL: none). */
static zend_always_inline zend_array *node_dyn(const gnode *n, const zend_object *o)
{
	return !n->dyn ? NULL : Z_TYPE(n->data) == IS_ARRAY ? Z_ARR(n->data) : o->properties;
}

/* ---- sizing objects and shared references ---- */

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

/* The dynamic properties without the slots. */
static void dyn_copy(zval *a, zend_object *o)
{
	array_init(a);
	zend_string *k;
	zend_ulong h;
	zval *v;
	ZEND_HASH_FOREACH_KEY_VAL(o->properties, h, k, v) {
		if (Z_TYPE_P(v) != IS_INDIRECT) {
			Z_TRY_ADDREF_P(v);
			if (k) {
				zend_hash_add_new(Z_ARR_P(a), k, v);
			} else {
				zend_hash_index_add_new(Z_ARR_P(a), h, v);
			}
		}
	} ZEND_HASH_FOREACH_END();
}

/* Calls __sleep() and finds its names as serialize() does (as given, private to the class, protected): h->named per slot,
 * the dynamic properties it names in h->data. SIZE_OBJECT for anything serialize() would warn about: serializing the value repeats the warning. */
static size_status sleep_props(zend_object *o, ghook *h)
{
	zend_class_entry *ce = o->ce;
	zval names;
	if (!call_magic(zend_hash_str_find_ptr(&ce->function_table, ZEND_STRL("__sleep")), o, &names)) {
		return SIZE_ERROR;
	}
	if (Z_TYPE(names) != IS_ARRAY) {
		zval_ptr_dtor(&names);
		return SIZE_OBJECT;
	}
	HashTable *props = zend_std_get_properties(o);
	uint8_t *named = ecalloc(MAX(ce->default_properties_count, 1), 1); /* never NULL: NULL means not called yet */
	HashTable *dyn = NULL;
	size_status st = SIZE_OK;
	zval *nm;
	ZEND_HASH_FOREACH_VAL(Z_ARR(names), nm) {
		ZVAL_DEREF(nm);
		if (Z_TYPE_P(nm) != IS_STRING) {
			st = SIZE_OBJECT;
			break;
		}
		zend_string *name = Z_STR_P(nm);
		zval *v = zend_hash_find(props, name);
		bool plain = v != NULL;
		for (int m = 0; !v && m < 2; m++) {
			zend_string *key = m ? zend_mangle_property_name("*", 1, ZSTR_VAL(name), ZSTR_LEN(name), 0)
				: zend_mangle_property_name(ZSTR_VAL(ce->name), ZSTR_LEN(ce->name), ZSTR_VAL(name), ZSTR_LEN(name), 0);
			v = zend_hash_find(props, key);
			zend_string_release(key);
		}
		if (v && Z_TYPE_P(v) == IS_INDIRECT) {
			zval *slot = Z_INDIRECT_P(v);
			uint32_t k = (uint32_t) (slot - o->properties_table);
			if (Z_TYPE_P(slot) == IS_UNDEF && ZEND_TYPE_IS_SET(ce->properties_info_table[k]->type)) {
				continue; /* uninitialized: serialize() leaves it out too */
			}
			if (Z_TYPE_P(slot) == IS_UNDEF || named[k]) {
				st = SIZE_OBJECT;
				break;
			}
			named[k] = 1;
		} else if (plain) {
			if (!dyn) {
				dyn = zend_new_array(0);
			}
			if (!zend_hash_add(dyn, name, v)) {
				st = SIZE_OBJECT;
				break;
			}
			Z_TRY_ADDREF_P(v);
		} else {
			st = SIZE_OBJECT;
			break;
		}
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(&names);
	if (st != SIZE_OK) {
		efree(named);
		if (dyn) {
			zend_array_release(dyn);
		}
		return st;
	}
	h->named = named;
	if (dyn) {
		ZVAL_ARR(&h->data, dyn);
	}
	return SIZE_OK;
}

/* o's node at the next index. false: o's __serialize/__sleep has not run yet, so its contents wait for the next pass. */
static bool object_node(graph_b *g, zend_object *o, uint32_t ck, gnode **out)
{
	uint32_t kind = ck & K_MASK;
	gnode *n = *out = graph_b_node(g);
	n->ck = ck;
	if (kind_sleep(kind) || kind == K_DATA) {
		const ghook *h = graph_b_res(g, o);
		ZVAL_COPY_VALUE(&n->data, &h->data);
		n->named = h->named;
		n->dyn = kind_sleep(kind) && Z_TYPE(h->data) == IS_ARRAY;
		return h->ran;
	}
	if (kind == K_DATE) {
		n->own = date_zone_name(o, &n->data);
		return true;
	}
	n->dyn = (kind == K_SLOTS || kind == K_WAKEUP) && o->properties && has_dynamic(o->properties);
	if (n->dyn && o->ce->default_properties_count) {
		dyn_copy(&n->data, o);
		n->own = true;
	}
	return true;
}

size_status nocopy_graph_b_hooks(graph_b *g)
{
	for (; g->nrun < g->nres; g->nrun++) {
		ghook *h = &g->res[g->nrun];
		zend_object *o = h->o;
		h->ran = true;
		if (o->ce->__serialize) {
			zval d;
			if (!call_magic(o->ce->__serialize, o, &d)) {
				return SIZE_ERROR;
			}
			if (Z_TYPE(d) != IS_ARRAY) {
				zval_ptr_dtor(&d);
				zend_type_error("%s::__serialize() must return an array", ZSTR_VAL(o->ce->name));
				return SIZE_ERROR;
			}
			ZVAL_COPY_VALUE(&h->data, &d);
		} else {
			size_status st = sleep_props(o, h);
			if (st != SIZE_OK) {
				return st;
			}
		}
	}
	return SIZE_OK;
}

__attribute__((noinline)) size_status nocopy_size_object(sizing *z, zend_object *o, int depth)
{
	graph_b *g = z->g;
	if (!g->on) {
		graph_b_init(g);
	}
	bool added;
	uint32_t i = pmap_add(&g->objs, o->handle, g->objs.n, &added)->v;
	if (!added) {
		return SIZE_OK; /* met before: a back reference */
	}
	zend_class_entry *ce = o->ce;
	uint32_t kind = fp_kind(class_fp(ce));
	if (!kind || zend_object_is_lazy(o) || (kind_slots(kind) && o->handlers != &std_object_handlers) || (kind == K_DATE && !date_native(o))) {
		return SIZE_OBJECT;
	}
	uint32_t ck = pmap_add(&g->cls, (uintptr_t) ce, g->cls.n << 3 | kind, &added)->v;
	if (added) {
		z->n += 2 * str_size(ZSTR_LEN(ce->name));
	}
	gnode *n;
	bool ready = object_node(g, o, ck, &n);
	/* read before sizing below: it may move the nodes */
	zval data = n->data;
	const uint8_t *named = n->named;
	zend_array *dyn = node_dyn(n, o);
	z->n += SALIGN(obj_size(kind_nprops(ce, kind) + (dyn != NULL)));
	size_status st = SIZE_OK;
	if (!ready) {
		return SIZE_OK; /* sized in the next pass */
	} else if (kind == K_ENUM) {
		size_vstr(z, Z_STR_P(zend_enum_fetch_case_name(o)));
	} else if (kind == K_DATA || kind == K_DATE) {
		st = nocopy_size_zval(z, &data, depth + 1);
	} else {
		for (int k = 0; k < ce->default_properties_count && st == SIZE_OK; k++) {
			if (!named || named[k]) {
				st = nocopy_size_zval(z, OBJ_PROP_NUM(o, k), depth + 1);
			}
		}
		if (dyn && st == SIZE_OK) {
			zval dv;
			ZVAL_ARR(&dv, dyn);
			st = nocopy_size_zval(z, &dv, depth + 1);
		}
	}
	if (st == SIZE_OK && kind_hook(kind)) {
		graph_b_hook(g, i);
	}
	return st;
}

/* Counts how often the value holds r: once, it is stored as its value; more, as a node. */
__attribute__((noinline)) size_status nocopy_size_ref(sizing *z, zend_reference *r, int depth)
{
	graph_b *g = z->g;
	if (!g->on) {
		graph_b_init(g);
	}
	if (!g->refs_on) {
		pmap_init(&g->refs);
		g->refs_on = true;
	}
	bool added;
	pslot *s = pmap_add(&g->refs, (uintptr_t) r, 1, &added);
	if (!added) {
		if (s->v++ == 1) {
			g->nref++;
		}
		return SIZE_OK;
	}
	return nocopy_size_zval(z, &r->val, depth + 1);
}

/* ---- persisting ---- */

__attribute__((noinline)) void nocopy_persist_object(persist *p, zval *z)
{
	zend_object *o = Z_OBJ_P(z);
	ZEND_ASSERT(p->g); /* objects make a graph */
	uint32_t i = pmap_probe(&p->g->objs, o->handle)->v;
	gnode *gn = &p->g->nodes[i];
	if (!gn->done) {
		gn->done = true;
		zend_class_entry *ce = o->ce;
		uint32_t kind = gn->ck & K_MASK;
		zend_array *dyn = node_dyn(gn, o);
		nc_obj *n = take(p, obj_size(kind_nprops(ce, kind) + (dyn != NULL)));
		graph_off(p->graph)[i] = (uint32_t) ((char *) n - (char *) p->graph);
		n->cls = gn->ck >> 3;
		n->dyn = dyn != NULL;
		if (kind == K_ENUM) {
			ZVAL_STR(&n->props[0], Z_STR_P(zend_enum_fetch_case_name(o)));
			nocopy_persist_zval(p, &n->props[0]);
		} else if (kind == K_DATA) {
			ZVAL_COPY_VALUE(&n->props[0], &gn->data);
			nocopy_persist_zval(p, &n->props[0]);
		} else if (kind == K_DATE) {
			const timelib_time *t = php_date_obj_from_obj(o)->time;
			ZVAL_LONG(&n->props[0], (zend_long) t->sse);
			ZVAL_LONG(&n->props[1], (zend_long) date_word(t));
			ZVAL_COPY_VALUE(&n->props[2], &gn->data);
			nocopy_persist_zval(p, &n->props[2]);
		} else {
			const uint8_t *named = gn->named;
			for (int k = 0; k < ce->default_properties_count; k++) {
				if (!named || named[k]) {
					ZVAL_COPY_VALUE(&n->props[k], OBJ_PROP_NUM(o, k));
					nocopy_persist_zval(p, &n->props[k]);
				} else {
					Z_TYPE_INFO(n->props[k]) = NC_DEFAULT;
				}
			}
			if (dyn) {
				zval *d = &n->props[ce->default_properties_count];
				ZVAL_ARR(d, dyn);
				nocopy_persist_zval(p, d);
			}
		}
	}
	Z_TYPE_INFO_P(z) = NC_OREF;
	Z_LVAL_P(z) = i;
	p->obj = true;
}

__attribute__((noinline)) bool nocopy_persist_ref(persist *p, zval *z)
{
	if (!p->g->refs_on) {
		return false;
	}
	pslot *s = pmap_probe(&p->g->refs, (uintptr_t) Z_REF_P(z));
	if (s->v == 1) {
		return false;
	}
	if (!(s->v & REF_DONE)) {
		s->v = REF_DONE | p->nref;
		zval *rv = &graph_refs(p->graph)[p->nref++];
		ZVAL_COPY_VALUE(rv, Z_REFVAL_P(z));
		bool outer = p->obj;
		nocopy_persist_zval(p, rv);
		p->obj = outer;
	}
	Z_LVAL_P(z) = s->v & ~REF_DONE;
	Z_TYPE_INFO_P(z) = NC_RREF;
	p->obj = true;
	return true;
}

__attribute__((noinline)) nc_graph *nocopy_persist_graph(persist *p, zval *val)
{
	graph_b *g = p->g;
	uint32_t ncls = g->cls.n, nobj = g->objs.n, nhooks = g->nh;
	nc_graph *gr = take(p, graph_head(ncls, g->nref, nobj, nhooks));
	gr->ncls = ncls;
	gr->nobj = nobj;
	gr->nref = g->nref;
	gr->nhooks = nhooks;
	if (nhooks) {
		memcpy(graph_hooks(gr), g->hooks, nhooks * sizeof(uint32_t));
	}
	for (uint32_t s = 0; s <= g->cls.mask; s++) {
		if (!g->cls.s[s].k) {
			continue;
		}
		zend_class_entry *ce = (zend_class_entry *) g->cls.s[s].k;
		nc_class *c = &graph_cls(gr)[g->cls.s[s].v >> 3];
		zend_string *lc = zend_string_tolower(ce->name);
		zend_string_hash_val(lc);
		c->name = nocopy_persist_str(p, ce->name);
		c->lc = nocopy_persist_str(p, lc);
		zend_string_release(lc);
		c->fp = class_fp(ce);
		c->nprops = kind_nprops(ce, fp_kind(c->fp));
	}
	p->graph = gr;
	ZVAL_COPY_VALUE(&gr->root, val);
	nocopy_persist_zval(p, &gr->root);
	return gr;
}

/* ---- moving ---- */

void nocopy_graph_rebase(zval *v, rebase *r)
{
	nc_graph *g = Z_PTR_P(v) = moved(r, Z_PTR_P(v));
	nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->ncls; i++) {
		c[i].name = moved(r, c[i].name);
		c[i].lc = moved(r, c[i].lc);
		ZSTR_H(c[i].lc) = 0;
	}
	graph_each(g, nocopy_rebase_zval, r);
}

/* ---- leaving shared memory ---- */

typedef enum {
	G_ZERO, /* values stay the arena's */
	G_COPY, /* copy mode: no arena pointer may escape */
	G_TPL,  /* from a template: its values are private, shared by refcount */
} gmode;

typedef struct {
	nocopy_keys *keys;
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

__attribute__((noinline)) static void graph_zval_slow(gout *o, zval *dst, const zval *src)
{
	switch (Z_TYPE_P(src)) {
		case NC_OREF:
			ZVAL_OBJ_COPY(dst, o->objs[Z_LVAL_P(src)]);
			break;
		case NC_SPINE: /* a private copy: objects and spines below get built, the rest stays shared or is copied */
			ZVAL_ARR(dst, zend_array_dup(Z_ARR_P(src))); /* a template's spine: dup addrefs its values */
			array_each(Z_ARR_P(dst), o->mode == G_COPY ? o->keys : NULL, graph_elem, o);
			break;
		case NC_RREF:
			GC_ADDREF(o->refs[Z_LVAL_P(src)]);
			ZVAL_REF(dst, o->refs[Z_LVAL_P(src)]);
			break;
		default:
			if (o->mode == G_COPY) {
				nocopy_copy_into(o->keys, dst, src);
			} else if (o->mode == G_TPL) {
				ZVAL_COPY(dst, src);
			} else {
				ZVAL_COPY_VALUE(dst, src);
			}
	}
}

/* The class table's entry (else autoload: user code), if its fingerprint still matches. */
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

/* Every object, empty (no constructor; enum cases looked up; dates whole). Returns how many were made: fewer if an enum case or a date's zone is gone. */
static uint32_t graph_objects_new(const nc_graph *g, zend_class_entry **ces, zend_object **objs)
{
	const nc_class *c = graph_cls(g);
	uint32_t i = 0;
	for (; i < g->nobj; i++) {
		nc_obj *n = graph_obj(g, i);
		zend_class_entry *ce = ces[n->cls];
		uint32_t kind = fp_kind(c[n->cls].fp);
		if (kind == K_ENUM) {
			zend_class_constant *cc = zend_hash_find_ptr(CE_CONSTANTS_TABLE(ce), Z_STR(n->props[0]));
			if (!cc || !(ZEND_CLASS_CONST_FLAGS(cc) & ZEND_CLASS_CONST_IS_CASE)) {
				break;
			}
			objs[i] = zend_enum_get_case(ce, Z_STR(n->props[0]));
			GC_ADDREF(objs[i]);
		} else if (kind == K_DATA || kind == K_DATE) {
			zval t;
			if (object_init_ex(&t, ce) != SUCCESS) {
				break;
			}
			if (kind == K_DATE && !date_out(Z_OBJ(t), n)) {
				zval_ptr_dtor(&t);
				break;
			}
			objs[i] = Z_OBJ(t);
		} else {
			objs[i] = zend_objects_new(ce);
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
	zval *dst = obj->properties_table, *def = obj->ce->default_properties_table;
	for (uint32_t k = 0; k < np; k++) {
		if (UNEXPECTED(Z_TYPE(n->props[k]) == NC_DEFAULT)) {
			ZVAL_COPY_OR_DUP_PROP(&dst[k], &def[k]);
		} else {
			graph_zval(o, &dst[k], &n->props[k]);
			Z_PROP_FLAG_P(&dst[k]) = Z_TYPE(dst[k]) == IS_UNDEF ? (Z_PROP_FLAG_P(&n->props[k]) & IS_PROP_UNINIT) : 0;
		}
	}
	if (n->dyn) {
		graph_dyn(o, obj, &n->props[np]);
	}
}

/* A typed slot holding a shared reference becomes one of its type sources. */
__attribute__((noinline)) static void graph_ref_types(const nc_graph *g, zend_object **objs)
{
	const nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->nobj; i++) {
		nc_obj *n = graph_obj(g, i);
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

/* The shared references' values, the objects' contents (K_DATA: data[i], for __unserialize()), the root. */
static void graph_fill(gout *o, const nc_graph *g, zval *data, zval *rv)
{
	const nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->nref; i++) {
		graph_zval(o, &o->refs[i]->val, &graph_refs(g)[i]);
	}
	for (uint32_t i = 0; i < g->nobj; i++) {
		nc_obj *n = graph_obj(g, i);
		uint32_t kind = fp_kind(c[n->cls].fp);
		if (kind == K_DATA) {
			graph_zval(o, &data[i], &n->props[0]);
		} else if (kind_slots(kind)) {
			graph_slots(o, o->objs[i], n, c[n->cls].nprops);
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

/* Reads g only before the hooks run user code. false: an enum case is gone, or a hook threw. */
static bool graph_build(zval *rv, const nc_graph *g, zend_class_entry **ces, gmode mode)
{
	zend_object *objs_s[16], **objs = SMALL_ALLOC(objs_s, g->nobj);
	zend_reference *refs_s[8], **refs = SMALL_ALLOC(refs_s, g->nref);
	uint32_t made = graph_objects_new(g, ces, objs), nref = 0;
	bool ok = made == g->nobj;
	if (ok) {
		gout o = {.keys = &NOCOPY_G(keys), .objs = objs, .refs = refs, .mode = mode};
		if (mode == G_COPY) {
			keys_reset(o.keys);
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
		for (uint32_t i = 0; i < made; i++) { /* never constructed: no destructor */
			if (!(objs[i]->ce->ce_flags & ZEND_ACC_ENUM)) {
				GC_ADD_FLAGS(objs[i], IS_OBJ_DESTRUCTOR_CALLED);
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

bool nocopy_graph_out(zval *rv, const zval *v, bool copy)
{
	const nc_graph *g = Z_PTR_P(v);
	zend_class_entry *ces_s[8], **ces = SMALL_ALLOC(ces_s, g->ncls);
	bool ok = graph_classes(g, ces) && graph_build(rv, g, ces, copy ? G_COPY : G_ZERO);
	SMALL_FREE(ces_s, ces);
	return ok;
}

/* ---- copy mode: a graph's private template, kept in the local table ---- */

/* A private copy of the graph's bytes (offsets stay valid), its values made private and its classes resolved. */
typedef struct {
	zend_class_entry **ces;
	nc_graph g; /* followed by the rest of the block */
} nc_tpl;

/* Private copy of an arena value, keeping graph markers: spines become private arrays. */
static void tpl_zval(void *keys, zval *z)
{
	if (Z_TYPE_P(z) == NC_SPINE) {
		zend_array *a = zend_array_dup(Z_ARR_P(z));
		array_each(a, keys, tpl_zval, keys);
		Z_ARR_P(z) = a;
	} else if (Z_TYPE_P(z) <= IS_ARRAY) {
		nocopy_unshare_zval(keys, z);
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
	graph_each(&t->g, tpl_zval, keys);
	return t;
}

void nocopy_graph_tpl_free(void *p)
{
	nc_tpl *t = p;
	graph_each(&t->g, tpl_free_zval, NULL);
	efree(t);
}

bool nocopy_graph_tpl_out(zval *rv, void *p)
{
	nc_tpl *t = p;
	return graph_build(rv, &t->g, t->ces, G_TPL);
}
