/* Internal to the value units: nocopy_value.c (plain values, the walks) and nocopy_graph.c (objects and shared references). */

#ifndef NOCOPY_VALUE_INT_H
#define NOCOPY_VALUE_INT_H

#include "nocopy_php.h"

#define NOCOPY_MAX_DEPTH 64
#define NOCOPY_VSTR_MAX 64 /* strings up to this length are stored once: per value (the key table), and arena-wide (atoms) */

/* buf if n elements fit, else request memory (SMALL_FREE). */
#define SMALL_ALLOC(buf, n) ((n) <= sizeof(buf) / sizeof *(buf) ? (buf) : emalloc((n) * sizeof *(buf)))
#define SMALL_FREE(buf, p) do { if ((p) != (buf)) efree(p); } while (0)
/* Room for one more in p[n]: cap doubles, from first. */
#define GROW(p, n, cap, first) do { if ((n) == (cap)) { (cap) = (cap) ? 2 * (cap) : (first); (p) = erealloc((p), (cap) * sizeof *(p)); } } while (0)
/* p grown to size bytes: out of inl (used bytes kept) into request memory, or reallocated there. */
static inline void *inl_grow(void *p, const void *inl, size_t used, size_t size) { return p == inl ? memcpy(emalloc(size), inl, used) : erealloc(p, size); }

typedef void (*zval_fn)(void *ctx, zval *z);

static zend_always_inline size_t str_size(size_t len) { return SALIGN(_ZSTR_STRUCT_SIZE(len)); }

#define ARENA_EMPTY (&NOCOPY_SHARED->empty)

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

/* ---- atoms: short strings stored once for the whole arena (nocopy_arena.h) ---- */

/* Met while sizing, not an atom: its second sighting admits it, and the atom is returned if this write may use it. */
zend_string *nocopy_atom_sight(const nocopy_atom_view *v, const zend_string *s, zend_ulong h);

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
	uint32_t ck;          /* class index << 3 | kind */
	bool dyn;             /* dynamic properties stored after the slots */
	bool done;            /* persisted */
	bool own;             /* data is the node's (dynamic properties copied); else borrowed from its ghook */
	zval data;            /* __serialize()'s array, else the dynamic properties without the slots; UNDEF: none, or the object's own table (stdClass) */
	const uint8_t *named; /* __sleep(): 1 per slot it named; NULL: no __sleep() */
} gnode;

/* A __serialize/__sleep result, kept by object across sizing passes. */
typedef struct {
	zend_object *o; /* held, so its handle stays unique */
	zval data;      /* __serialize()'s array, or the dynamic properties __sleep() named */
	uint8_t *named;
	bool ran;
} ghook;

#define GHOOK_INL 4 /* hook results found by a linear search, without a map */

/* One value's objects, classes and shared references, in the order sizing meets them (persisting walks the same order). */
typedef struct {
	pmap objs;       /* handle -> index */
	pmap cls;        /* class entry -> index << 3 | kind */
	gnode *nodes;    /* by index */
	uint32_t nn, ncap;
	uint32_t *hooks; /* indices, inner first */
	uint32_t nh, hcap;
	pmap refs;       /* shared reference -> times met while sizing; while persisting REF_DONE | index */
	uint32_t nref;   /* met more than once: nodes */
	pmap *hk;        /* handle -> index into res, past GHOOK_INL; outlives a pass */
	ghook *res;
	uint32_t nres, nrun, rcap;
	bool on;         /* set up: an object or a shared reference was met */
	bool refs_on;
	gnode inl[16];
	ghook rinl[GHOOK_INL];
} graph_b;

/* Not = {0}: zeroing its tables cost every store ~3 ns. */
static zend_always_inline void graph_b_off(graph_b *g) { g->on = false; }
static zend_always_inline bool graph_b_used(const graph_b *g) { return g->on && (g->objs.n || g->nref); }
/* Sizing met objects whose __serialize/__sleep has not run: run them (nocopy_graph_b_hooks), then size again. */
static zend_always_inline bool graph_b_pending(const graph_b *g) { return g->on && g->nrun < g->nres; }

/* ---- sizing ---- */

typedef enum { SIZE_OK, SIZE_DEEP, SIZE_OBJECT, SIZE_RESOURCE, SIZE_ERROR /* exception thrown */ } size_status;

typedef struct {
	size_t n;         /* out-of-line bytes persisting will consume */
	nocopy_keys keys; /* this value's view of the table (after keys_reset) */
	graph_b *g;
	const nocopy_atom_view *av; /* NULL: no atoms */
	bool sight;       /* first pass: strings that are not atoms count towards admission */
	bool atoms;       /* an atom was used */
	/* per short string met first in the value (a table miss), in walk order: its atom or NULL. Persisting replays it, looking nothing up */
	zend_string **found; /* the caller's buffer of cap, until it grows into request memory (heap) */
	uint32_t nfound, cap;
	bool heap;
} sizing;

void nocopy_found_grow(sizing *z);
static zend_always_inline void sizing_free(sizing *z)
{
	if (z->heap) {
		efree(z->found);
		z->heap = false;
	}
}

size_status nocopy_size_zval(sizing *z, zval *v, int depth);

/* A key, or a short string inside an array or object: counted once per value, and not at all if it is an atom (persist_key decides alike).
 * The value's table first: the shared one is probed once per distinct string. */
static zend_always_inline void size_str(sizing *z, zend_string *s, zend_ulong h)
{
	nocopy_kslot *k;
	if (key_seen(&z->keys, s, h, &k)) {
		return;
	}
	if (z->av && ZSTR_LEN(s) <= NOCOPY_VSTR_MAX) {
		zend_string *a = nocopy_atom_find(z->av, h, ZSTR_VAL(s), (uint32_t) ZSTR_LEN(s));
		if (!a && z->sight) {
			a = nocopy_atom_sight(z->av, s, h);
		}
		if (UNEXPECTED(z->nfound == z->cap)) {
			nocopy_found_grow(z);
		}
		z->found[z->nfound++] = a;
		if (a) {
			z->atoms = true;
			return;
		}
	}
	z->n += str_size(ZSTR_LEN(s));
}

/* A string inside an array or object (persist_vstr decides alike). */
static zend_always_inline void size_vstr(sizing *z, zend_string *s)
{
	if (ZSTR_LEN(s) > NOCOPY_VSTR_MAX) {
		z->n += str_size(ZSTR_LEN(s));
	} else {
		size_str(z, s, zend_string_hash_val(s));
	}
}

/* ---- persisting: values into an arena block, opcache-style immutable ---- */

typedef struct {
	char *cursor;    /* next free byte of the block */
	nocopy_keys keys;
	zend_string *const *found; /* sizing's (NULL: no atoms), replayed in the same order */
	uint32_t fi;
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
	uint8_t atoms; /* atom halves the block references */
} rebase;

/* p repointed into the moved block; ARENA_EMPTY and atoms stay (atoms are noted). */
static zend_always_inline void *moved(rebase *r, void *p)
{
	if ((uintptr_t) p - r->from < r->size) {
		return (char *) p + r->by;
	}
	r->atoms |= nocopy_atom_bits(p);
	return p;
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
size_status nocopy_graph_b_hooks(graph_b *g); /* user code: only between sizing passes */
void nocopy_graph_b_rewind(graph_b *g);     /* for the next sizing: only the hooks' results stay */
void nocopy_graph_b_free(graph_b *g);

nc_graph *nocopy_persist_graph(persist *p, zval *val);
void nocopy_persist_object(persist *p, zval *z); /* *z becomes NC_OREF; the object's node is filled the first time */
bool nocopy_persist_ref(persist *p, zval *z);    /* *z becomes NC_RREF; false: held once, store it as its value */

void nocopy_graph_rebase(zval *v, rebase *r);

#endif
