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

/* More holes than elements: written as a map of its elements only (a packed one too, its indices as keys), as unserialize() would build it. */
static zend_always_inline bool sparse(const zend_array *a) { return a->nNumUsed - a->nNumOfElements > 8 && a->nNumUsed > 2 * a->nNumOfElements; }

static zend_always_inline uint32_t sparse_mask(const zend_array *a)
{
	uint32_t slots = 4;
	while (slots < 2 * a->nNumOfElements) {
		slots <<= 1;
	}
	return (uint32_t) -(int32_t) slots;
}

/* The bytes of a's data in the block. */
static zend_always_inline size_t data_size(const zend_array *a)
{
	if (UNEXPECTED(sparse(a))) {
		return HT_HASH_SIZE(sparse_mask(a)) + a->nNumOfElements * sizeof(Bucket);
	}
	return HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : map_size(a);
}

/* ---- the string table: equal strings stored once per value ---- */

#define NC_STRTAB 256

struct nocopy_strslot {
	const zend_string *s;
	zend_string *copy; /* reading: its private copy */
	zend_ulong h;
	uint32_t gen;
	uint32_t ord; /* building: its index in nc_builder.str */
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

/* Followed by nc_class[ncls], zval refs[nref] (shared references' values), uint32_t off[nobj] (each object's offset from the graph, signed:
 * the header is built last, after the objects), uint32_t hooks[nhooks]. */
typedef struct {
	uint32_t ncls, nobj, nhooks, nref;
	zval root;
} nc_graph;

static zend_always_inline nc_class *graph_cls(const nc_graph *g) { return (nc_class *) (g + 1); }
static zend_always_inline zval *graph_refs(const nc_graph *g) { return (zval *) (graph_cls(g) + g->ncls); }
static zend_always_inline uint32_t *graph_off(const nc_graph *g) { return (uint32_t *) (graph_refs(g) + g->nref); }
static zend_always_inline nc_obj *graph_obj(const nc_graph *g, uint32_t i) { return (nc_obj *) ((char *) g + (int32_t) graph_off(g)[i]); }
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

/* ---- building (nocopy_build.c): one walk into a private buffer; user code may run ---- */

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

/* What building learned about one object. */
typedef struct {
	zend_object *o;       /* held: its handle stays unique while user code runs */
	uint32_t ck;          /* class index << 3 | kind */
	uint32_t off;         /* its nc_obj's block offset */
	bool dyn;             /* dynamic properties stored after the slots */
	bool own;             /* data is the node's (released with it); else borrowed from its hook result */
	zval data;            /* __serialize()'s array, else the dynamic properties without the slots; UNDEF: none, or the object's own table (stdClass) */
	const uint8_t *named; /* __sleep(): 1 per slot it named; NULL: no __sleep() */
} gnode;

/* A __serialize/__sleep result, kept by object across passes. */
typedef struct {
	zend_object *o; /* held, so its handle stays unique */
	zval data;      /* __serialize()'s array, or the dynamic properties __sleep() named */
	uint8_t *named;
	bool ran;
} ghook;

/* A class of the graph, by index. */
typedef struct {
	zend_class_entry *ce;
	zend_string *lc; /* lowercase name, hashed */
	uint64_t fp;
} gclass;

#define GHOOK_INL 4 /* hook results found by a linear search, without a map */

/* One value's objects, classes and shared references, by index in the order building meets them. */
typedef struct {
	pmap objs;       /* handle -> index */
	pmap cls;        /* class entry -> index << 3 | kind */
	gclass *classes; /* by index */
	uint32_t ccap;
	gnode *nodes;    /* by index */
	uint32_t nn, ncap;
	uint32_t *hooks; /* indices, inner first */
	uint32_t nh, hcap;
	pmap refs;       /* reference (held) -> the pass + 1 that met it, or REF_MULTI | index; outlives a pass */
	uint32_t nref;   /* held more than once: nodes */
	zval *refv;      /* their built values */
	uint8_t *refdone; /* built in this pass */
	uint32_t vcap;
	pmap *hk;        /* handle -> index into res, past GHOOK_INL; outlives a pass */
	ghook *res;
	uint32_t nres, rcap;
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

/* Only a long value string (or a string alone) may be unhashed in a block: such a string is never in the string table, and rebase zeroes exactly these. */
static zend_always_inline bool nc_may_be_unhashed(const zend_string *s) { return ZSTR_LEN(s) > NC_VSTR_MAX; }
#define STR_INL 32
#define HV_INL 16

/* An array of a region, in building's order: its hash (0: not shareable), the arrays of its subtree with itself, bytes at most. */
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
	nc_cand *cand;
	uint32_t ncand, ccap;
	hnode inl[HV_INL];
} nc_share;

static zend_always_inline nc_share *nc_share_init(nc_share *s)
{
	s->hv = s->inl;
	s->hn = s->hc = s->hend = 0;
	s->hcap = HV_INL;
	s->ncand = s->ccap = 0;
	s->cand = NULL;
	return s;
}

typedef enum { BUILD_OK, BUILD_DEEP, BUILD_OBJECT, BUILD_RESOURCE, BUILD_ERROR /* an exception is pending */, BUILD_AGAIN /* internal: a new shared reference */ } build_status;

/* A source array the walk is reading, and whether it holds a reference to it (taken when user code runs). */
typedef struct {
	zend_array *a;
	bool held;
} nc_open;
#define NC_OPEN_MAX (NC_MAX_DEPTH + 2)

/* A value built in one walk into a private buffer: pointers into it are block offsets (base: the block offset of buf[0]),
 * every such pointer's place listed in rel, so placing it is a copy and one add per pointer. */
typedef struct {
	nocopy_env *env;
	nocopy_strtab strs; /* this value's view of the table */
	graph_b *g;
	const nocopy_atom_view *av; /* NULL: no atoms */
	nc_share *sh;      /* NULL: arrays are not shared (no atoms) */
	bool sight;        /* first pass: strings that are not atoms count towards admission */
	bool atoms;        /* an atom is used */
	bool obj;          /* an object or shared reference met since the innermost array began */
	nocopy_fmt fmt;
	uint32_t pass;
	zend_string **str; /* per distinct deduped string: its atom or its copy */
	uint32_t ns, scap;
	zend_string **str_inl;
	size_t base;
	char *buf;         /* env's, taken while building */
	size_t bn, bcap;
	uint32_t *rel;
	uint32_t rn, rcap;
	zend_array **held; /* references taken for user code, released at the end */
	uint32_t nheld, hcap, cheld;
	uint32_t nopen;
	nc_open open[NC_OPEN_MAX];
	zval root;
} nc_builder;

#define NC_IS_OFF(p) ((uintptr_t) (p) < ((uintptr_t) 1 << 32))
/* A built zval that points into the buffer (an offset to relocate). */
static zend_always_inline bool nc_off_val(const zval *v)
{
	return ((1u << (Z_TYPE_P(v) & 31)) & (1u << IS_STRING | 1u << IS_ARRAY | 1u << NC_SPINE | 1u << IS_PTR)) && NC_IS_OFF(Z_PTR_P(v));
}

void nc_build_init(nc_builder *b, nocopy_env *env, const nocopy_atom_view *av, nc_share *ss, graph_b *g, zend_string **str, size_t base);
build_status nc_build(nc_builder *b, zval *val); /* b->root, b->fmt: plain or graph */
void nc_build_bufs_back(nc_builder *b); /* the buffers to env, once placed */
void nc_build_free(nc_builder *b);      /* may run destructors */
bool nc_same_array(const zend_array *a, const zend_array *b, int depth); /* equal keys, order, types and values (doubles bitwise) */
void nc_share_admit(nocopy_env *env, const nc_builder *outer, const nocopy_atom_view *av); /* its candidates the doorkeeper lets in */

/* ---- placing (nocopy_write.c): under a shard lock, so no allocation and no user code ---- */

/* A scalar, or a string alone at p (str_size bytes). */
static zend_always_inline void nc_write_flat(nocopy_entry *e, char *p, const zval *val)
{
	zval *v = block_val(e);
	if (Z_TYPE_P(val) != IS_STRING) {
		ZVAL_COPY_VALUE(v, val);
		return;
	}
	const zend_string *s = Z_STR_P(val);
	zend_string *r = (zend_string *) p;
	memcpy(r, s, _ZSTR_STRUCT_SIZE(ZSTR_LEN(s)));
	GC_SET_REFCOUNT(r, 2);
	GC_TYPE_INFO(r) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	ZVAL_STR(v, r);
	Z_TYPE_FLAGS_P(v) = 0;
}
void nc_place_at(char *p, const nc_builder *b); /* b's buffer at p + base, its offsets made pointers from p */
void nc_place(nocopy_entry *e, const nc_builder *b); /* b's value into e, after its header and key */

/* ---- reading (nocopy_read.c) ---- */

bool nc_graph_out(nocopy_env *env, zval *rv, const zval *v, bool copy); /* false: a class is gone or changed, a hook threw */

#endif
