/* Internal to the value layer's units: what the phases share (plan -> write, the graph's layout, classes). */

#ifndef NOCOPY_VALUE_INT_H
#define NOCOPY_VALUE_INT_H

#include "nocopy_value.h"
#include "ext/date/php_date.h"

#define NC_MAX_DEPTH 64
#define NC_VSTR_MAX 64 /* strings up to this length are stored once: per value (the string table), and arena-wide (atoms) */
#define NC_SHARE_MIN 128      /* bytes an array takes before an equal one is shared arena-wide */
#define NC_SHARE_MAX 65536    /* larger arrays are not admitted */
#define NC_SHARE_STR 256      /* longer strings are not hashed: arrays holding them are not shared */
#define NC_SHARE_ADMIT 64     /* arrays a write may admit */

#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)
#define KEY_OFF SALIGN(sizeof(nocopy_block)) /* the key's bytes: only compared, never a zend_string */
_Static_assert(KEY_OFF <= UINT8_MAX, "koff is 8 bits");

/* buf if n elements fit, else request memory (SMALL_FREE). */
#define SMALL_ALLOC(buf, n) ((n) <= sizeof(buf) / sizeof *(buf) ? (buf) : emalloc((n) * sizeof *(buf)))
#define SMALL_FREE(buf, p) do { if ((p) != (buf)) efree(p); } while (0)
/* Room for one more in p[n]: cap doubles, from first. */
#define GROW(p, n, cap, first) do { if ((n) == (cap)) { (cap) = (cap) ? 2 * (cap) : (first); (p) = erealloc((p), (cap) * sizeof *(p)); } } while (0)
/* p grown to size bytes: out of inl (used bytes kept) into request memory, or reallocated there. */
static inline void *inl_grow(void *p, const void *inl, size_t used, size_t size) { return p == inl ? memcpy(emalloc(size), inl, used) : erealloc(p, size); }

/* Checked in a debug build of PHP and in ours with -DNOCOPY_DEBUG (./dev asan, fuzz, debug): a release PHP compiles ZEND_ASSERT out. */
#if ZEND_DEBUG || defined(NOCOPY_DEBUG)
# define NC_CHECKS 1
# define NC_ASSERT(c) do { if (UNEXPECTED(!(c))) { fprintf(stderr, "nocopy: assertion failed: %s (%s:%d)\n", #c, __FILE__, __LINE__); abort(); } } while (0)
#else
# define NC_CHECKS 0
# define NC_ASSERT(c) ZEND_ASSUME(c) /* as ZEND_ASSERT does in a release build */
#endif

typedef void (*zval_fn)(void *ctx, zval *z);

static zend_always_inline size_t str_size(size_t len) { return SALIGN(_ZSTR_STRUCT_SIZE(len)); }

extern nocopy_value_shared *nc_shared;
#define ARENA_EMPTY (&nc_shared->empty)

static zend_always_inline zval *block_val(nocopy_entry *e) { return &((nocopy_block *) e)->val; }

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

/* ---- the string table: equal strings stored once per value ---- */

#define NC_STRTAB 256

struct nocopy_strslot {
	const zend_string *s;
	zend_string *copy; /* reading: its private copy */
	zend_ulong h;
	uint32_t gen;
	uint32_t ord; /* planning: its index in plan.str */
};

static zend_always_inline void strtab_reset(nocopy_strtab *t)
{
	if (UNEXPECTED(++t->gen == 0)) {
		memset(t->slot, 0, NC_STRTAB * sizeof(nocopy_strslot));
		t->gen = 1;
	}
}

static zend_always_inline nocopy_strslot *strtab_slot(nocopy_strtab *t, zend_ulong h) { return &t->slot[h & (NC_STRTAB - 1)]; }

static zend_always_inline void strtab_put(const nocopy_strtab *t, nocopy_strslot *s, const zend_string *k, zend_ulong h)
{
	s->s = k;
	s->h = h;
	s->gen = t->gen;
}

/* ---- graph values: what a NOCOPY_FMT_GRAPH block holds besides plain values ---- */

/* Graph values are plain (at most IS_ARRAY) or one of these; none reaches userland. */
#define NC_OREF 0x18    /* Z_LVAL: the object's index */
#define NC_RREF 0x19    /* Z_LVAL: the shared reference's index */
#define NC_SPINE 0x1a   /* Z_ARR: an array with objects or shared references below it, rebuilt on fetch */
#define NC_DEFAULT 0x1b /* a slot __sleep() left out: the class's default value */
_Static_assert(NC_OREF > IS_PTR && NC_OREF > _IS_ERROR, "graph markers are no PHP type");

/* How a class's objects are stored (the low bits of its fingerprint). */
typedef enum {
	K_NONE,   /* not as a node: the value is serialized */
	K_SLOTS,  /* the raw property slots */
	K_WAKEUP, /* the slots, then __wakeup() */
	K_SLEEP,  /* the slots __sleep() names, NC_DEFAULT in the others */
	K_SLEEPW, /* K_SLEEP, then __wakeup() */
	K_DATA,   /* props[0]: __serialize()'s array, given to __unserialize() */
	K_ENUM,   /* props[0]: the case's name */
	K_DATE,   /* DateTime(Immutable): props[0] timestamp, [1] date_word(), [2] zone ID or abbreviation */
} nc_kind;
#define K_MASK 7

static zend_always_inline nc_kind fp_kind(uint64_t fp) { return (nc_kind) (fp & K_MASK); }
static zend_always_inline bool kind_slots(nc_kind k) { return k >= K_SLOTS && k <= K_SLEEPW; }
static zend_always_inline bool kind_sleep(nc_kind k) { return k == K_SLEEP || k == K_SLEEPW; }
static zend_always_inline bool kind_hook(nc_kind k) { return k == K_WAKEUP || k == K_SLEEPW || k == K_DATA; }
static zend_always_inline uint32_t kind_nprops(const zend_class_entry *ce, nc_kind k) { return kind_slots(k) ? (uint32_t) ce->default_properties_count : k == K_DATE ? 3 : 1; }

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
typedef struct {
	uint32_t ncls, nobj, nhooks, nref;
	zval root;
} nc_graph;

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

/* ---- classes (nocopy_class.c) ---- */

uint64_t nc_class_fp(nocopy_env *env, zend_class_entry *ce); /* kind | hash of what must not change; cached per request */
zend_class_entry *nc_class_resolve(nocopy_env *env, const nc_class *c); /* NULL: gone (autoload is user code) or changed */
bool nc_call_magic(zend_function *f, zend_object *o, zval *rv); /* false: it threw (rv released) */
bool nc_date_native(zend_object *o);           /* stored as K_DATE: initialized, with a zone, no dynamic properties */
bool nc_date_zone_name(zend_object *o, zval *z); /* its zone's ID or abbreviation (false: null, an offset) */
bool nc_date_out(nocopy_env *env, zend_object *obj, const nc_obj *n); /* false: the zone is gone */

/* microseconds | zone type << 20 | dst << 24 | UTC offset << 32 */
static zend_always_inline uint64_t date_word(const timelib_time *t)
{
	return (uint64_t) t->us | (uint64_t) t->zone_type << 20 | (uint64_t) (t->dst != 0) << 24 | (uint64_t) (uint32_t) t->z << 32;
}

/* ---- planning: sizing the value and taking every decision writing replays ---- */

/* Nonzero key -> uint32, open addressing, at most half full; small graphs stay in inl. */
typedef struct {
	uintptr_t k;
	uint32_t v;
} pslot;

typedef struct {
	pslot *s;
	uint32_t mask, n;
	pslot inl[32];
} pmap;


/* What planning learned about one object. */
typedef struct {
	uint32_t ck;          /* class index << 3 | kind */
	bool dyn;             /* dynamic properties stored after the slots */
	bool done;            /* written */
	bool own;             /* data is the node's (released with it); else borrowed from its hook result */
	zval data;            /* __serialize()'s array, else the dynamic properties without the slots; UNDEF: none, or the object's own table (stdClass) */
	const uint8_t *named; /* __sleep(): 1 per slot it named; NULL: no __sleep() */
} gnode;

/* A __serialize/__sleep result, kept by object across planning passes. */
typedef struct {
	zend_object *o; /* held, so its handle stays unique */
	zval data;      /* __serialize()'s array, or the dynamic properties __sleep() named */
	uint8_t *named;
	bool ran;
} ghook;

/* A class of the graph, by index. */
typedef struct {
	zend_class_entry *ce;
	zend_string *lc; /* lowercase name, hashed: made while planning, as writing must not allocate */
	uint64_t fp;
} gclass;

#define GHOOK_INL 4 /* hook results found by a linear search, without a map */

/* One value's objects, classes and shared references, by index in the order planning meets them. */
typedef struct {
	pmap objs;       /* handle -> index */
	pmap cls;        /* class entry -> index << 3 | kind */
	gclass *classes; /* by index */
	uint32_t ccap;
	gnode *nodes;    /* by index */
	uint32_t nn, ncap;
	uint32_t *hooks; /* indices, inner first */
	uint32_t nh, hcap;
	pmap refs;       /* shared reference -> its first tape position, or REF_MULTI | index */
	uint32_t nref;   /* held more than once: nodes */
	pmap *hk;        /* handle -> index into res, past GHOOK_INL; outlives a pass */
	ghook *res;
	uint32_t nres, nrun, rcap;
	bool on;         /* set up: an object or a shared reference was met */
	bool refs_on;
	gnode inl[16];
	gclass cinl[4];
	ghook rinl[GHOOK_INL];
} graph_b;

/* The dynamic properties stored after the slots (NULL: none). */
static zend_always_inline zend_array *node_dyn(const gnode *n, const zend_object *o)
{
	return !n->dyn ? NULL : Z_TYPE(n->data) == IS_ARRAY ? Z_ARR(n->data) : o->properties;
}

/* Not = {0}: zeroing its tables cost every store ~3 ns. */
static zend_always_inline void graph_b_off(graph_b *g) { g->on = false; }
static zend_always_inline bool graph_b_used(const graph_b *g) { return g->on && (g->objs.n || g->nref); }
void nc_graph_b_free(graph_b *g); /* leaves it off; may run destructors: never under a shard lock */
static zend_always_inline void graph_b_release(graph_b *g)
{
	if (g->on) {
		nc_graph_b_free(g);
	}
}

/* The tape: one entry per decision, in walk order.
 * A deduped string: its index in str << 1 | T_FIRST the first time. An object: its index.
 * A reference held by more than one zval: T_ONCE (the value holds it once: written as its value), else its index << 1 | T_FIRST. */
#define T_FIRST 1u
typedef enum { OP_KEY, OP_STR, OP_LSTR, OP_ARR, OP_EMPTY, OP_SHARED, OP_OBJ, OP_OBJ_REF, OP_DEREF, OP_REF, OP_REF_USE } nc_opk;
#define NC_OP_MAX 0xFFFFFFFu /* 28 bits of payload */
/* Only a long value string (or a string alone) may be unhashed in a block: such a string is never in the string table, and rebase zeroes exactly these. */
static zend_always_inline bool nc_may_be_unhashed(const zend_string *s) { return ZSTR_LEN(s) > NC_VSTR_MAX; }
#define OP(k, x) ((uint32_t) (k) | (uint32_t) (x) << 4)
#define TAPE_INL 64
#define STR_INL 32

#define HV_INL 16

/* An array of a region, in planning's order: its hash (0: not shareable), the arrays of its subtree with itself, bytes at most. */
typedef struct {
	uint64_t h;
	uint32_t n, up;
} hnode;

/* An array no shared one equals yet: admitted after the commit if the doorkeeper has seen it before. */
typedef struct {
	zend_array *a;
	uint64_t h;
} nc_cand;

/* Sharing arrays: set up by array and object writes only (scalars do not zero it). */
typedef struct {
	hnode *hv;         /* the regions' arrays: the current one [hc, hend), nested ones above it up to hn */
	uint32_t hn, hc, hend, hcap;
	zend_array **arr;  /* the shared arrays the tape points at */
	uint32_t narr, acap;
	nc_cand *cand;
	uint32_t ncand, ccap;
	hnode inl[HV_INL];
	zend_array *arr_inl[8];
} nc_share;

static zend_always_inline nc_share *nc_share_init(nc_share *s)
{
	s->hv = s->inl;
	s->hn = s->hc = s->hend = 0;
	s->hcap = HV_INL;
	s->arr = s->arr_inl;
	s->acap = 8;
	s->narr = s->ncand = s->ccap = 0;
	s->cand = NULL;
	return s;
}

typedef struct {
	nocopy_env *env;
	size_t n;          /* the bytes writing will take after the key */
	nocopy_strtab strs; /* this value's view of the table */
	graph_b *g;
	const nocopy_atom_view *av; /* NULL: no atoms */
	bool sight;        /* first pass: strings that are not atoms count towards admission */
	bool atoms;        /* an atom is used */
	uint32_t *tape;
	uint32_t nt, tcap;
	zend_string **str; /* per distinct deduped string: its atom, else NULL until writing puts its copy there */
	uint32_t ns, scap;
	uint32_t *tape_inl; /* the caller's buffers, until they grow into request memory */
	zend_string **str_inl;
	nc_share *sh;      /* NULL: arrays are not shared (no atoms) */
} plan;

typedef enum { PLAN_OK, PLAN_DEEP, PLAN_OBJECT, PLAN_RESOURCE, PLAN_ERROR /* an exception is pending */ } plan_status;

plan_status nc_plan_walk(plan *z, zval *val); /* above IS_STRING: arrays, objects, references; runs hooks between passes */

/* Scalars and a string alone inline: a string alone bypasses the table, writing stores it alike. */
static zend_always_inline plan_status nc_plan_value(plan *z, zval *val)
{
	if (Z_TYPE_P(val) < IS_STRING) {
		return PLAN_OK;
	}
	if (Z_TYPE_P(val) == IS_STRING) {
		z->n += str_size(Z_STRLEN_P(val));
		return PLAN_OK;
	}
	return nc_plan_walk(z, val);
}
void nc_plan_free(plan *z);
bool nc_same_array(const zend_array *a, const zend_array *b, int depth); /* equal keys, order, types and values (doubles bitwise) */
void nc_share_admit(nocopy_env *env, const plan *outer, const nocopy_atom_view *av); /* its candidates the doorkeeper lets in */

/* ---- writing (nocopy_write.c): under a shard lock, so no allocation and no user code ---- */

/* A value to write: what planning produced. */
typedef struct {
	plan z;
	graph_b g;
	nocopy_fmt fmt;
	zval *val; /* the value, or ser */
	zval ser;  /* NOCOPY_FMT_PHP/IGB: the serialized string */
} encoding;

static zend_always_inline size_t encoding_size(const encoding *x, size_t klen) { return SALIGN(sizeof(nocopy_block)) + SALIGN(klen) + x->z.n; }
bool nc_write(encoding *x, nocopy_entry *e); /* fills e after its header and key: exactly encoding_size bytes */
bool nc_write_array(const plan *z, const zend_array *src, void *dst); /* a shared array at dst: exactly z->n bytes */


/* ---- reading (nocopy_read.c) ---- */

bool nc_graph_out(nocopy_env *env, zval *rv, const zval *v, bool copy); /* false: a class is gone or changed, a hook threw */

#endif
