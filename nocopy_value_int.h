/* Internal to the value layer's units: what the phases share (build -> place, the graph's layout, classes). */

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
#define NC_OFF_MAX ((size_t) 64 << 20) /* bytes a value is built to without an arena (its checks only) */

#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)
#define KEY_OFF SALIGN(sizeof(nocopy_block)) /* the key's bytes: only compared, never a zend_string */
_Static_assert(KEY_OFF <= UINT8_MAX, "koff is 8 bits");
/* Where the value's out-of-line parts start: after the header, the zval and the key. */
static zend_always_inline size_t val_off(uint32_t klen) { return KEY_OFF + SALIGN(klen); }

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
/* An arena string's type: interned, its UTF-8 flag kept, never IS_STR_PERMANENT (process lifetime: PCRE would keep the pointer). */
static zend_always_inline uint32_t arena_str_type(const zend_string *s) { return GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0); }

extern nocopy_value_shared *nc_shared;
#define ARENA_EMPTY (&nc_shared->empty)

static zend_always_inline zval *block_val(nocopy_entry *e) { return &((nocopy_block *) e)->val; }

/* The smallest power of two from min up that holds n (a loop: __builtin_clz cost small maps more, design-decisions). */
static zend_always_inline uint32_t pow2_at_least(uint32_t min, uint32_t n)
{
	uint32_t p = min;
	while (p < n) {
		p <<= 1;
	}
	return p;
}

/* A map's hash in the arena: 2 slots per bucket rounded up, as PHP sizes it, not the table's 16 minimum. */
static zend_always_inline uint32_t map_mask(const zend_array *a)
{
	uint32_t slots = pow2_at_least(4, 2 * a->nNumUsed);
	return slots < (uint32_t) -(int32_t) a->nTableMask ? (uint32_t) -(int32_t) slots : a->nTableMask;
}

static zend_always_inline size_t map_size(const zend_array *a) { return HT_HASH_SIZE(map_mask(a)) + a->nNumUsed * sizeof(Bucket); }

/* More holes than elements: written as a map of its elements only (a packed one too, its indices as keys), as unserialize() would build it. */
static zend_always_inline bool sparse(const zend_array *a) { return a->nNumUsed - a->nNumOfElements > 8 && a->nNumUsed > 2 * a->nNumOfElements; }

static zend_always_inline uint32_t sparse_mask(const zend_array *a) { return (uint32_t) -(int32_t) pow2_at_least(4, 2 * a->nNumOfElements); }

/* The bytes of a's data in the block. */
static zend_always_inline size_t data_size(const zend_array *a)
{
	if (UNEXPECTED(sparse(a))) {
		return HT_HASH_SIZE(sparse_mask(a)) + a->nNumOfElements * sizeof(Bucket);
	}
	return HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : map_size(a);
}

/* The bytes of a in the block, keys and values apart. */
static zend_always_inline size_t array_bytes(const zend_array *a) { return SALIGN(sizeof(zend_array)) + SALIGN(data_size(a)); }

/* ---- the string table: equal strings stored once per value ---- */

#define NC_STRTAB 256

struct nocopy_strslot {
	const zend_string *s;
	zend_string *copy; /* reading: its private copy */
	zend_ulong h;
	uint32_t gen;
	uint32_t ord; /* building: its index in the builder's strings */
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

/* A K_DATE node's props[1]: microseconds | zone type << DW_TYPE | dst << DW_DST | UTC offset << DW_OFF */
#define DW_TYPE 20
#define DW_DST 24
#define DW_OFF 32
static zend_always_inline uint64_t date_word(const timelib_time *t)
{
	return (uint64_t) t->us | (uint64_t) t->zone_type << DW_TYPE | (uint64_t) (t->dst != 0) << DW_DST | (uint64_t) (uint32_t) t->z << DW_OFF;
}
static zend_always_inline int date_word_us(uint64_t w) { return (int) (w & ((1u << DW_TYPE) - 1)); }
static zend_always_inline int date_word_type(uint64_t w) { return (int) (w >> DW_TYPE) & 0xf; }
static zend_always_inline int date_word_dst(uint64_t w) { return (int) (w >> DW_DST) & 1; }
static zend_always_inline int32_t date_word_offset(uint64_t w) { return (int32_t) (w >> DW_OFF); }

/* ---- building (nocopy_build.c): one walk into a private buffer; user code may run ---- */

/* Only a long value string (or a string alone) may be unhashed in a block: such a string is never in the string table, and rebase zeroes exactly these. */
static zend_always_inline bool nc_may_be_unhashed(const zend_string *s) { return ZSTR_LEN(s) > NC_VSTR_MAX; }

#define NC_IS_OFF(p) ((uintptr_t) (p) < ((uintptr_t) 1 << 32))
/* A built zval that points into the buffer (an offset to relocate). */
static zend_always_inline bool nc_off_val(const zval *v)
{
	return ((1u << (Z_TYPE_P(v) & 31)) & (1u << IS_STRING | 1u << IS_ARRAY | 1u << NC_SPINE | 1u << IS_PTR)) && NC_IS_OFF(Z_PTR_P(v));
}

/* A built value as placing sees it: buf's bn bytes go at block offset base, rel lists the offsets of its pointer slots. */
typedef struct {
	const char *buf;
	size_t base, bn;
	const uint32_t *rel;
	uint32_t rn;
	zval root;
	uint8_t atoms; /* atom halves it may point into (checks) */
} nc_built;

/* Builds val and writes it as q's entry; serialized if it cannot be built. User code runs before the arena is touched, destructors after the commit. */
nocopy_status nc_build_put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only, const nocopy_atom_view *av);
bool nc_build_check(nocopy_env *env, zval *val); /* false: a write would refuse val */
#if NC_CHECKS
void nc_share_check(char *p, const nc_built *b); /* a shared array just placed at p: its record, empty arrays and atoms of b's view only */
#endif

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
	GC_TYPE_INFO(r) = arena_str_type(s);
	ZVAL_STR(v, r);
	Z_TYPE_FLAGS_P(v) = 0;
}
/* ---- writing a flat value (nocopy_value.c, nocopy_build.c) ---- */

static zend_always_inline nocopy_status nc_put_result(nocopy_put_result c)
{
	return c == NOCOPY_PUT_OK ? NOCOPY_OK : c == NOCOPY_PUT_EXISTS ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
}

/* A scalar or a string alone (a serialized value too): nothing to build or release. */
static zend_always_inline nocopy_status nc_put_flat(nocopy_ctx *ctx, nocopy_put_req *q, const zval *val, bool add_only, nocopy_fmt fmt)
{
	q->size = val_off(q->klen) + (Z_TYPE_P(val) == IS_STRING ? str_size(Z_STRLEN_P(val)) : 0);
	q->koff = KEY_OFF;
	q->flags = (uint8_t) fmt;
	q->atoms = false;
	nocopy_put p;
	nocopy_entry *e = nocopy_put_begin(&p, ctx, q);
	if (!e) {
		return NOCOPY_NO_ROOM;
	}
	nc_write_flat(e, (char *) e + val_off(q->klen), val);
	return nc_put_result(nocopy_put_commit(&p, add_only));
}

void nc_place_at(char *p, const nc_built *b); /* b's buffer at p + base, its offsets made pointers from p */
void nc_place(nocopy_entry *e, const nc_built *b); /* b's value into e, after its header and key */

/* ---- reading (nocopy_read.c) ---- */

bool nc_graph_out(nocopy_env *env, zval *rv, const zval *v, bool copy); /* false: a class is gone or changed, a hook threw */

#endif
