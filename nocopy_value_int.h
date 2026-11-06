/* Internal to the value units: nocopy_value.c (plain values, the walks) and nocopy_graph.c (objects and shared references). */

#ifndef NOCOPY_VALUE_INT_H
#define NOCOPY_VALUE_INT_H

#include "nocopy_php.h"

#define NOCOPY_MAX_DEPTH 64
#define NOCOPY_VSTR_MAX 64 /* value strings up to this length share the key table: stored once per value */

/* buf if n elements fit, else request memory (SMALL_FREE). */
#define SMALL_ALLOC(buf, n) ((n) <= sizeof(buf) / sizeof *(buf) ? (buf) : emalloc((n) * sizeof *(buf)))
#define SMALL_FREE(buf, p) do { if ((p) != (buf)) efree(p); } while (0)

typedef void (*zval_fn)(void *ctx, zval *z);

static zend_always_inline size_t str_size(size_t len) { return SALIGN(_ZSTR_STRUCT_SIZE(len)); }

/* The empty array every entry points to: in the arena, so it has the same address in every process that maps it (unlike zend_empty_array). */
#define ARENA_EMPTY ((zend_array *) nocopy_a->user)

static zend_always_inline bool in_arena(const void *p) { return (uintptr_t) p - (uintptr_t) nocopy_a < nocopy_a->size; }

/* ---- the key table: equal strings stored once per value ---- */

static zend_always_inline nocopy_kslot *key_slot(nocopy_keys *t, zend_ulong h) { return &t->slot[h & (NOCOPY_KEYS - 1)]; }

static zend_always_inline void key_put(const nocopy_keys *t, nocopy_kslot *s, const zend_string *k, zend_ulong h)
{
	s->s = k;
	s->h = h;
	s->gen = t->gen;
}

/* True if an equal string came earlier in this value; else it is recorded. *slot: its slot either way.
 * Depends only on the sequence of string contents, so sizing and persisting agree. */
static zend_always_inline bool key_seen(nocopy_keys *t, const zend_string *k, zend_ulong h, nocopy_kslot **slot)
{
	nocopy_kslot *s = *slot = key_slot(t, h);
	if (s->gen == t->gen && s->h == h && zend_string_equals(s->s, k)) {
		return true;
	}
	key_put(t, s, k, h);
	return false;
}

/* ---- graph values: what an ENTRY_GRAPH block holds besides plain values ---- */

#define NC_OREF IS_OBJECT      /* Z_LVAL is the object's index */
#define NC_RREF IS_REFERENCE   /* Z_LVAL is the shared reference's index */
#define NC_SPINE IS_PTR        /* an array with objects or shared references below it, copied on fetch */
#define NC_DEFAULT IS_INDIRECT /* in a slot __sleep() left out: the class's default value */
/* Graph values are plain (at most IS_ARRAY) or one of the markers above. */

typedef struct nc_graph nc_graph; /* the block's layout: nocopy_graph.c */

/* Nonzero key -> uint32, open addressing, at most half full (igbinary's hash_si_ptr); small graphs stay in inl. */
typedef struct {
	uintptr_t k;
	uint32_t v;
} pslot;

typedef struct {
	pslot *s;
	uint32_t mask, n;
	pslot inl[32];
} pmap;

/* What sizing learned about one object, so persisting looks nothing up again. */
typedef struct {
	uint32_t ck;    /* class index << 3 | kind */
	bool dyn;       /* dynamic properties stored after the slots */
	bool done;      /* persisted */
	zval data;      /* __serialize()'s array (called once), else the dynamic properties without the slots; UNDEF: none, or the object's own table (stdClass) */
	uint8_t *named; /* __sleep(): 1 per slot it named; NULL: not called, or no __sleep() */
} gnode;

/* One value's objects, classes and shared references, in the order sizing meets them (persisting walks the same order). */
typedef struct {
	pmap objs;       /* handle -> index */
	pmap cls;        /* class entry -> index << 3 | kind */
	gnode *nodes;    /* by index; kept for a second sizing */
	uint32_t nn, ncap;
	uint32_t *hooks; /* indices, inner first */
	uint32_t nh, hcap;
	pmap refs;       /* shared reference -> times met while sizing; while persisting REF_DONE | index */
	uint32_t nref;   /* met more than once: nodes */
	bool on;         /* set up: an object or a shared reference was met */
	bool refs_on;
	bool user;       /* __serialize/__sleep ran: they may have used the key table, so sizing is done again */
	gnode inl[16];
} graph_b;

/* Not = {0}: zeroing its tables cost every store ~3 ns. */
static zend_always_inline void graph_b_off(graph_b *g) { g->on = g->user = false; }
static zend_always_inline bool graph_b_used(const graph_b *g) { return g->on && (g->objs.n || g->nref); }

/* ---- sizing ---- */

typedef enum { SIZE_OK, SIZE_DEEP, SIZE_OBJECT, SIZE_RESOURCE, SIZE_ERROR /* exception thrown */ } size_status;

typedef struct {
	size_t n;         /* out-of-line bytes persisting will consume */
	nocopy_keys keys; /* this value's view of the table (after keys_reset) */
	graph_b *g;
} sizing;

size_status nocopy_size_zval(sizing *z, zval *v, int depth);

/* A string inside an array or object: counted once per value if short (persist_vstr decides alike). */
static zend_always_inline void size_vstr(sizing *z, zend_string *s)
{
	nocopy_kslot *k;
	if (ZSTR_LEN(s) > NOCOPY_VSTR_MAX || !key_seen(&z->keys, s, zend_string_hash_val(s), &k)) {
		z->n += str_size(ZSTR_LEN(s));
	}
}

/* ---- persisting: values into an arena block, opcache-style immutable ---- */

typedef struct {
	char *cursor;    /* next free byte of the block */
	nocopy_keys keys;
	graph_b *g;      /* NULL: no objects or shared references */
	nc_graph *graph;
	uint32_t nref;   /* shared references persisted so far */
	bool obj;        /* an object or shared reference met since the innermost array began */
} persist;

static zend_always_inline void *take(persist *p, size_t n)
{
	void *r = p->cursor;
	p->cursor += SALIGN(n);
	return r;
}

zend_string *nocopy_persist_str(persist *p, const zend_string *s);
void nocopy_persist_zval(persist *p, zval *z); /* rewrites *z in place so everything it points to lives in the block */

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

void nocopy_rebase_zval(void *r, zval *z); /* r: rebase */

/* ---- leaving shared memory ---- */

void nocopy_copy_into(nocopy_keys *keys, zval *dst, const zval *src); /* deep private copy; keys shares the deduped keys' copies */
void nocopy_unshare_zval(void *keys, zval *z); /* an arena string or array inside a private array: replaced by a private copy */
void nocopy_unshare_key(nocopy_keys *keys, zend_array *a, Bucket *b);

/* Each value of a private copy of an arena array through f; with keys, the keys still in the arena get private copies first. */
static zend_always_inline void array_each(zend_array *a, nocopy_keys *keys, zval_fn f, void *ctx)
{
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			f(ctx, v);
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (keys && b->key && in_arena(b->key)) {
				nocopy_unshare_key(keys, a, b);
			}
			f(ctx, &b->val);
		} ZEND_HASH_FOREACH_END();
	}
}

/* ---- graphs (nocopy_graph.c) ---- */

/* Sizing meets an object, or a reference held more than once. SIZE_OBJECT: no node for it, so the whole value is serialized. */
size_status nocopy_size_object(sizing *z, zend_object *o, int depth);
size_status nocopy_size_ref(sizing *z, zend_reference *r, int depth);
size_t nocopy_graph_head(const graph_b *g); /* the graph's fixed part, aligned */
void nocopy_graph_b_rewind(graph_b *g);     /* for a second sizing: the nodes (and __serialize's arrays) stay, the walk starts over */
void nocopy_graph_b_free(graph_b *g);

nc_graph *nocopy_persist_graph(persist *p, zval *val);
void nocopy_persist_object(persist *p, zval *z); /* *z becomes NC_OREF; the object's node is filled the first time */
bool nocopy_persist_ref(persist *p, zval *z);    /* *z becomes NC_RREF; false: held once, store it as its value */

void nocopy_graph_rebase(zval *v, rebase *r);

#endif
