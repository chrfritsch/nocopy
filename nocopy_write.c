/* nocopy: placing a built value into its block, and repointing a moved block.
 * Runs under a shard lock: no allocation, no user code (./dev layers checks). */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"

#if NC_CHECKS
/* Everything a block (or a shared array's record) points to lies in it, is the arena's empty array or an atom of an allowed half, and is immutable. */
typedef struct {
	const char *base;
	size_t size;
	const zval *root; /* the block's own value: a string alone may be stored unhashed */
	uint8_t atoms;    /* halves it may point into */
	bool shared;      /* a shared array: no objects or references */
} checker;

static bool inside(const checker *c, const void *p) { return (uintptr_t) p - (uintptr_t) c->base < c->size; }

static void check_ptr(const checker *c, const void *p, size_t n)
{
	NC_ASSERT(inside(c, p) && n <= c->size - ((uintptr_t) p - (uintptr_t) c->base));
}

static void check_str(const checker *c, const zend_string *s)
{
	if (inside(c, s)) {
		check_ptr(c, s, _ZSTR_HEADER_SIZE);
		check_ptr(c, s, _ZSTR_STRUCT_SIZE(ZSTR_LEN(s)));
	} else {
		NC_ASSERT(nocopy_atom_bits(s) & c->atoms);
	}
	NC_ASSERT(ZSTR_IS_INTERNED(s) && !(GC_FLAGS(s) & IS_STR_PERMANENT) && GC_REFCOUNT(s) >= 2 && ZSTR_VAL(s)[ZSTR_LEN(s)] == 0);
}

static void check_zval(void *ctx, zval *z)
{
	const checker *c = ctx;
	switch (Z_TYPE_P(z)) {
		case IS_STRING:
			NC_ASSERT(!Z_TYPE_FLAGS_P(z));
			check_str(c, Z_STR_P(z));
			NC_ASSERT(nc_may_be_unhashed(Z_STR_P(z)) || z == c->root || ZSTR_H(Z_STR_P(z)));
			break;
		case NC_SPINE:
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(z);
			NC_ASSERT(!Z_TYPE_FLAGS_P(z) && (Z_TYPE_P(z) == IS_ARRAY || !c->shared));
			if (a == ARENA_EMPTY) {
				break;
			}
			if (!inside(c, a)) {
				NC_ASSERT(Z_TYPE_P(z) == IS_ARRAY && (nocopy_atom_bits(a) & c->atoms) && (GC_FLAGS(a) & IS_ARRAY_IMMUTABLE) && GC_REFCOUNT(a) == 2);
				break; /* a shared array: checked when admitted (nc_share_check) */
			}
			check_ptr(c, a, sizeof *a);
			NC_ASSERT((GC_FLAGS(a) & IS_ARRAY_IMMUTABLE) && GC_REFCOUNT(a) == 2 && !a->pDestructor && zend_hash_num_elements(a));
			check_ptr(c, HT_GET_DATA_ADDR(a), HT_IS_PACKED(a) ? HT_PACKED_USED_SIZE(a) : HT_USED_SIZE(a));
			if (HT_IS_PACKED(a)) {
				zval *v;
				ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
					check_zval(ctx, v);
				} ZEND_HASH_FOREACH_END();
			} else {
				Bucket *b;
				ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
					if (b->key) {
						check_str(c, b->key);
						NC_ASSERT(ZSTR_H(b->key) == b->h && zend_hash_find_known_hash(a, b->key) == &b->val);
					}
					check_zval(ctx, &b->val);
				} ZEND_HASH_FOREACH_END();
			}
			break;
		}
		case NC_OREF: case NC_RREF: case NC_DEFAULT:
			NC_ASSERT(!c->shared);
			break;
		default:
			NC_ASSERT(Z_TYPE_P(z) < IS_STRING);
	}
}

static void block_check(nocopy_entry *e, uint8_t atoms)
{
	zval *v = block_val(e);
	checker c = {.base = (const char *) e, .size = nocopy_entry_size(e), .root = v, .atoms = atoms};
	if (nocopy_value_fmt(e) != NOCOPY_FMT_GRAPH) {
		NC_ASSERT(Z_TYPE_P(v) <= IS_ARRAY);
		check_zval(&c, v);
		return;
	}
	nc_graph *g = Z_PTR_P(v);
	check_ptr(&c, g, graph_head(g->ncls, g->nref, g->nobj, g->nhooks));
	for (uint32_t i = 0; i < g->ncls; i++) {
		check_str(&c, graph_cls(g)[i].name);
		check_str(&c, graph_cls(g)[i].lc);
	}
	for (uint32_t i = 0; i < g->nobj; i++) {
		check_ptr(&c, graph_obj(g, i), obj_size(0));
		NC_ASSERT(graph_obj(g, i)->cls < g->ncls);
		check_ptr(&c, graph_obj(g, i), obj_size(obj_nz(graph_cls(g), graph_obj(g, i))));
	}
	for (uint32_t i = 0; i < g->nhooks; i++) {
		NC_ASSERT(graph_hooks(g)[i] < g->nobj);
	}
	graph_each(g, check_zval, &c);
}

void nc_share_check(char *p, const nc_builder *b)
{
	checker c = {.base = p, .size = b->bn, .atoms = nocopy_atom_bit(b->av), .shared = true};
	zval v;
	Z_ARR(v) = (zend_array *) (p + (uintptr_t) Z_ARR(b->root));
	Z_TYPE_INFO(v) = IS_ARRAY; /* immutable, as a block stores it: ZVAL_ARR would add the refcounted flags */
	check_zval(&c, &v);
}
#endif

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

static void rebase_zval(void *ctx, zval *z)
{
	rebase *r = ctx;
	switch (Z_TYPE_P(z)) {
		case IS_STRING: {
			zend_string *s = moved(r, Z_STR_P(z));
			if (s != Z_STR_P(z)) {
				Z_STR_P(z) = s;
				if (nc_may_be_unhashed(s)) { /* shorter ones were hashed before writing: no reader writes theirs, and keys may share them */
					ZSTR_H(s) = 0; /* readers may have been hashing the old copy while it was copied: rehash on use */
				}
			}
			break;
		}
		case NC_SPINE:
		case IS_ARRAY: {
			zend_array *a = moved(r, Z_ARR_P(z));
			if (a == Z_ARR_P(z)) {
				break; /* did not move (the empty array, a shared one): its atoms are noted, nothing of it is ours */
			}
			Z_ARR_P(z) = a;
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

static void rebase_graph(zval *v, rebase *r)
{
	nc_graph *g = Z_PTR_P(v) = moved(r, Z_PTR_P(v));
	nc_class *c = graph_cls(g);
	for (uint32_t i = 0; i < g->ncls; i++) {
		c[i].name = moved(r, c[i].name);
		c[i].lc = moved(r, c[i].lc);
	}
	graph_each(g, rebase_zval, r);
}

void nc_place_at(char *p, const nc_builder *b)
{
	if (b->bn) {
		memcpy(p + b->base, b->buf, b->bn);
	}
	for (uint32_t i = 0; i < b->rn; i++) {
		*(uintptr_t *) (p + b->rel[i]) += (uintptr_t) p;
	}
}

void nc_place(nocopy_entry *e, const nc_builder *b)
{
	char *p = (char *) e;
	nc_place_at(p, b);
	zval *v = block_val(e);
	ZVAL_COPY_VALUE(v, &b->root);
	if (nc_off_val(v)) {
		Z_PTR_P(v) = p + (uintptr_t) Z_PTR_P(v);
	}
#if NC_CHECKS
	block_check(e, b->av && b->atoms ? nocopy_atom_bit(b->av) : 0);
#endif
}

uint8_t nocopy_value_rebase(nocopy_entry *e, const nocopy_entry *from)
{
	rebase r = {.from = (uintptr_t) from, .size = nocopy_entry_size(from), .by = (char *) e - (const char *) from};
	if (nocopy_value_fmt(e) == NOCOPY_FMT_GRAPH) {
		rebase_graph(block_val(e), &r);
	} else {
		zval *v = block_val(e);
		rebase_zval(&r, v);
		if (Z_TYPE_P(v) == IS_STRING && (uintptr_t) Z_STR_P(v) - (uintptr_t) e < nocopy_entry_size(e)) {
			ZSTR_H(Z_STR_P(v)) = 0; /* a string alone was stored as it came, maybe unhashed, at any length */
		}
	}
#if NC_CHECKS
	block_check(e, 3); /* moved: its segment takes the halves it points into */
#endif
	return r.atoms;
}
