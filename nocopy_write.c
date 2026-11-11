/* nocopy: writing a planned value into its block, opcache-style immutable, and repointing a moved block.
 * Runs under a shard lock: no allocation, no user code (./dev layers checks). Every decision comes from the plan's tape. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "zend_enum.h"

#ifdef __clang__ /* no __builtin_setjmp on every clang target */
# include <setjmp.h>
# define BAIL_SET(b) _setjmp(b)
# define BAIL_GO(b) longjmp(b, 1)
#else
# define BAIL_SET(b) __builtin_setjmp(b)
# define BAIL_GO(b) __builtin_longjmp(b, 1)
#endif

typedef struct {
	char *cursor;        /* next free byte of the block */
	char *end;
	const uint32_t *tape;
	uint32_t ti, nt, ns, narr;
	zend_string **str;   /* the plan's: a deduped string's atom, or its copy once written */
	graph_b *g;
	nc_graph *graph;
	zend_array *const *arr; /* the plan's shared arrays */
	bool obj;            /* an object or shared reference met since the innermost array began */
#ifdef __clang__
	jmp_buf bail;
#else
	void *bail[5];
#endif
} writer;

/* The tape and the block disagree: leaves the write; the caller aborts the put. */
static __attribute__((noinline, noreturn, cold)) void bad(writer *w)
{
	BAIL_GO(w->bail);
}
#define NEED(w, c) do { if (UNEXPECTED(!(c))) { bad(w); } } while (0)

static zend_always_inline void *take(writer *w, size_t n)
{
	void *r = w->cursor;
	NEED(w, (size_t) (w->end - w->cursor) >= SALIGN(n));
	w->cursor += SALIGN(n);
	return r;
}

static zend_always_inline uint32_t op_next(writer *w)
{
	NEED(w, w->ti < w->nt);
	return w->tape[w->ti++];
}

/* ---- strings ---- */

static zend_string *write_copy(writer *w, const zend_string *s)
{
	size_t size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(s));
	zend_string *r = take(w, size);
	memcpy(r, s, size); /* keys arrive hashed; values as planning hashed them, or unhashed (benign idempotent write on use) */
	GC_SET_REFCOUNT(r, 2);
	/* not IS_STR_PERMANENT: that promises process lifetime (PCRE then keeps the pointer as a cache key) */
	GC_TYPE_INFO(r) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	return r;
}

/* A deduped string: its first occurrence written (or its atom), later ones share it. */
static zend_always_inline zend_string *write_str(writer *w, const zend_string *s, uint32_t t, bool value)
{
	NEED(w, t >> 1 < w->ns);
	zend_string **slot = &w->str[t >> 1];
	if (t & T_FIRST) {
		if (!*slot) {
			*slot = write_copy(w, s);
		}
		return *slot;
	}
	zend_string *r = *slot;
	NEED(w, r);
#if NC_CHECKS
	NC_ASSERT(zend_string_equals(r, s)); /* an op that fits the slot's type but belongs to another slot */
#endif
	if (value && ZSTR_LEN(r) > 1 && !nocopy_atom_bits(r)) {
		GC_SET_REFCOUNT(r, 3); /* a value used more than once in the block: copy-out shares one copy (shorter: PHP's interned; atoms have it) */
	}
	return r;
}

/* ---- plain values ---- */

/* The table's data into the block; a map's hash shrunk to map_mask, its buckets rechained as opcache compacts. */
static void write_data(writer *w, zend_array *ht, const zend_array *src)
{
	if (HT_IS_PACKED(src) || (ht->nTableMask = map_mask(src)) == src->nTableMask) {
		size_t size = HT_IS_PACKED(src) ? HT_PACKED_USED_SIZE(src) : HT_USED_SIZE(src);
		void *data = take(w, size);
		memcpy(data, HT_GET_DATA_ADDR(src), size);
		HT_SET_DATA_ADDR(ht, data);
		return;
	}
	HT_SET_DATA_ADDR(ht, take(w, map_size(src)));
	Bucket *d = ht->arData;
	const Bucket *sb = src->arData;
	uint32_t mask = ht->nTableMask;
	for (uint32_t *h = (uint32_t *) d - (uint32_t) -(int32_t) mask; h < (uint32_t *) d; h++) {
		*h = HT_INVALID_IDX;
	}
	for (uint32_t i = 0; i < ht->nNumUsed; i++) {
		d[i] = sb[i];
		if (EXPECTED(Z_TYPE(d[i].val) != IS_UNDEF)) {
			uint32_t ni = (uint32_t) d[i].h | mask;
			Z_NEXT(d[i].val) = HT_HASH_EX(d, ni);
			HT_HASH_EX(d, ni) = HT_IDX_TO_HASH(i);
		}
	}
}

static void write_zval(writer *w, zval *z);

static zend_array *write_array(writer *w, const zend_array *src)
{
	zend_array *ht = take(w, sizeof(zend_array));
	memcpy(ht, src, sizeof(zend_array));
	ht->pDestructor = NULL; /* a function address: differs between processes; immutable arrays are never destroyed */
	write_data(w, ht, src);
	ht->nInternalPointer = 0;
	GC_SET_REFCOUNT(ht, 2);
	GC_TYPE_INFO(ht) = GC_ARRAY | IS_ARRAY_IMMUTABLE;

	/* serialization does not keep the next free index, so neither do we: max int key + 1 */
	zend_long max = ZEND_LONG_MIN;
	if (HT_IS_PACKED(ht)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(ht, v) {
			write_zval(w, v);
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
				uint32_t t = op_next(w);
				NEED(w, (t & 15) == OP_KEY);
				b->key = write_str(w, b->key, t >> 4, false);
			} else if ((zend_long) b->h > max) {
				max = (zend_long) b->h;
			}
			write_zval(w, &b->val);
		} ZEND_HASH_FOREACH_END();
	}
	ht->nNextFreeElement = (max == ZEND_LONG_MIN || max == ZEND_LONG_MAX) ? max : max + 1;
	return ht;
}

/* ---- objects and shared references ---- */

static void write_node(writer *w, const gnode *gn, zend_object *o, nc_obj *n, zend_array *dyn)
{
	nc_kind kind = (nc_kind) (gn->ck & K_MASK);
	switch (kind) {
		case K_ENUM:
			ZVAL_STR(&n->props[0], Z_STR_P(zend_enum_fetch_case_name(o)));
			write_zval(w, &n->props[0]);
			return;
		case K_DATA:
			ZVAL_COPY_VALUE(&n->props[0], &gn->data);
			write_zval(w, &n->props[0]);
			return;
		case K_DATE: {
			const timelib_time *t = php_date_obj_from_obj(o)->time;
			ZVAL_LONG(&n->props[0], (zend_long) t->sse);
			ZVAL_LONG(&n->props[1], (zend_long) date_word(t));
			ZVAL_COPY_VALUE(&n->props[2], &gn->data);
			write_zval(w, &n->props[2]);
			return;
		}
		case K_SLOTS: case K_WAKEUP: case K_SLEEP: case K_SLEEPW: {
			int np = o->ce->default_properties_count;
			for (int k = 0; k < np; k++) {
				if (!gn->named || gn->named[k]) {
					ZVAL_COPY_VALUE(&n->props[k], OBJ_PROP_NUM(o, k));
					write_zval(w, &n->props[k]);
				} else {
					Z_TYPE_INFO(n->props[k]) = NC_DEFAULT;
				}
			}
			if (dyn) {
				ZVAL_ARR(&n->props[np], dyn);
				write_zval(w, &n->props[np]);
			}
			return;
		}
		case K_NONE:
			break;
	}
	bad(w);
}

/* *z becomes NC_OREF; first: its node is written. */
static __attribute__((noinline)) void write_object(writer *w, zval *z, uint32_t i, bool first)
{
	NEED(w, w->graph && i < w->g->nn);
	zend_object *o = Z_OBJ_P(z);
	gnode *gn = &w->g->nodes[i];
	if (first) {
		zend_array *dyn = node_dyn(gn, o);
		nc_obj *n = take(w, obj_size(kind_nprops(o->ce, (nc_kind) (gn->ck & K_MASK)) + (dyn != NULL)));
		graph_off(w->graph)[i] = (uint32_t) ((char *) n - (char *) w->graph);
		n->cls = gn->ck >> 3;
		n->dyn = dyn != NULL;
		write_node(w, gn, o, n, dyn);
	}
	Z_TYPE_INFO_P(z) = NC_OREF;
	Z_LVAL_P(z) = i;
	w->obj = true;
}

/* *z becomes NC_RREF; first: its value is written into the graph's refs. */
static __attribute__((noinline)) void write_ref(writer *w, zval *z, uint32_t i, bool first)
{
	NEED(w, w->graph && i < w->g->nref);
	if (first) {
		zval *rv = &graph_refs(w->graph)[i];
		ZVAL_COPY_VALUE(rv, Z_REFVAL_P(z));
		bool outer = w->obj;
		write_zval(w, rv);
		w->obj = outer;
	}
	Z_TYPE_INFO_P(z) = NC_RREF;
	Z_LVAL_P(z) = i;
	w->obj = true;
}

/* Rewrites *z in place so everything it points to lives in the block: one op per value that is no scalar, checked against it. */
static void write_zval(writer *w, zval *z)
{
	if (Z_TYPE_P(z) < IS_STRING) {
		return;
	}
	uint32_t t = op_next(w), x = t >> 4;
	switch ((nc_opk) (t & 15)) {
		case OP_STR:
			NEED(w, Z_TYPE_P(z) == IS_STRING);
			Z_STR_P(z) = write_str(w, Z_STR_P(z), x, true);
			Z_TYPE_FLAGS_P(z) = 0;
			return;
		case OP_LSTR:
			NEED(w, Z_TYPE_P(z) == IS_STRING);
			Z_STR_P(z) = write_copy(w, Z_STR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			return;
		case OP_EMPTY:
			NEED(w, Z_TYPE_P(z) == IS_ARRAY);
			Z_ARR_P(z) = ARENA_EMPTY;
			Z_TYPE_FLAGS_P(z) = 0;
			return;
		case OP_SHARED:
			NEED(w, Z_TYPE_P(z) == IS_ARRAY && x < w->narr);
			Z_ARR_P(z) = w->arr[x];
			Z_TYPE_FLAGS_P(z) = 0;
			return;
		case OP_ARR: {
			NEED(w, Z_TYPE_P(z) == IS_ARRAY && (Z_ARRVAL_P(z)->nNumUsed & NC_OP_MAX) == x);
			bool outer = w->obj;
			w->obj = false;
			Z_ARR_P(z) = write_array(w, Z_ARR_P(z));
			Z_TYPE_FLAGS_P(z) = 0;
			if (w->obj) {
				Z_TYPE_INFO_P(z) = NC_SPINE;
			}
			w->obj |= outer;
			return;
		}
		case OP_OBJ: case OP_OBJ_REF:
			NEED(w, Z_TYPE_P(z) == IS_OBJECT);
			write_object(w, z, x, (t & 15) == OP_OBJ);
			return;
		case OP_DEREF:
			NEED(w, Z_TYPE_P(z) == IS_REFERENCE);
			ZVAL_COPY_VALUE(z, Z_REFVAL_P(z));
			write_zval(w, z);
			return;
		case OP_REF: case OP_REF_USE:
			NEED(w, Z_TYPE_P(z) == IS_REFERENCE);
			write_ref(w, z, x, (t & 15) == OP_REF);
			return;
		case OP_KEY:
			break;
	}
	bad(w);
}

static __attribute__((noinline)) nc_graph *write_graph(writer *w, zval *val)
{
	const graph_b *g = w->g;
	nc_graph *gr = take(w, graph_head(g->cls.n, g->nref, g->objs.n, g->nh));
	gr->ncls = g->cls.n;
	gr->nobj = g->objs.n;
	gr->nref = g->nref;
	gr->nhooks = g->nh;
	if (g->nh) {
		memcpy(graph_hooks(gr), g->hooks, g->nh * sizeof(uint32_t));
	}
	for (uint32_t i = 0; i < g->cls.n; i++) {
		const gclass *gc = &g->classes[i];
		nc_class *c = &graph_cls(gr)[i];
		c->name = write_copy(w, gc->ce->name);
		c->lc = write_copy(w, gc->lc);
		c->fp = gc->fp;
		c->nprops = kind_nprops(gc->ce, fp_kind(gc->fp));
	}
	w->graph = gr;
	ZVAL_COPY_VALUE(&gr->root, val);
	write_zval(w, &gr->root);
	return gr;
}

#if NC_CHECKS
/* Everything the block points to lies in the block, is the arena's empty array or an atom, and is immutable. */
typedef struct {
	const nocopy_entry *e;
} checker;

static void check_ptr(const checker *c, const void *p, size_t n)
{
	NC_ASSERT((uintptr_t) p - (uintptr_t) c->e < nocopy_entry_size(c->e) && n <= nocopy_entry_size(c->e) - ((uintptr_t) p - (uintptr_t) c->e));
}

static void check_str(const checker *c, const zend_string *s)
{
	if (!nocopy_atom_bits(s)) {
		check_ptr(c, s, _ZSTR_HEADER_SIZE);
		check_ptr(c, s, _ZSTR_STRUCT_SIZE(ZSTR_LEN(s)));
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
			NC_ASSERT(nc_may_be_unhashed(Z_STR_P(z)) || z == block_val((nocopy_entry *) c->e) || ZSTR_H(Z_STR_P(z)));
			break;
		case NC_SPINE:
		case IS_ARRAY: {
			zend_array *a = Z_ARR_P(z);
			NC_ASSERT(!Z_TYPE_FLAGS_P(z));
			if (a == ARENA_EMPTY) {
				break;
			}
			if (nocopy_atom_bits(a)) {
				NC_ASSERT(Z_TYPE_P(z) == IS_ARRAY && (GC_FLAGS(a) & IS_ARRAY_IMMUTABLE) && GC_REFCOUNT(a) == 2);
				break; /* a shared array: checked when admitted */
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
			break;
		default:
			NC_ASSERT(Z_TYPE_P(z) < IS_STRING);
	}
}

static void block_check(nocopy_entry *e)
{
	checker c = {.e = e};
	zval *v = block_val(e);
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
#endif

/* Arrays, objects, references: the tape's ops, each checked; false: the tape and the value disagree. Out of line: the bail-out's setup stays off the scalar path. */
static __attribute__((noinline)) bool write_tape(writer *w, encoding *x, zval *v)
{
	if (BAIL_SET(w->bail)) {
		return false;
	}
#if NC_CHECKS
	if (w->nt && getenv("NOCOPY_FAULT_TAPE")) {
		w->nt--; /* test 103: the tape one op short */
	}
#endif
	if (x->fmt == NOCOPY_FMT_GRAPH) {
		ZVAL_PTR(v, write_graph(w, x->val));
	} else {
		ZVAL_COPY_VALUE(v, x->val);
		write_zval(w, v);
	}
	NEED(w, w->cursor == w->end && w->ti == w->nt); /* written == planned, the tape consumed */
	return true;
}

bool nc_write(encoding *x, nocopy_entry *e)
{
	char *p = (char *) e + SALIGN(sizeof(nocopy_block)) + SALIGN(nocopy_entry_klen(e)), *end = (char *) e + nocopy_entry_size(e); /* the key's bytes: nocopy_put_begin copied them */
	zval *v = block_val(e);
	if (x->fmt != NOCOPY_FMT_GRAPH && Z_TYPE_P(x->val) <= IS_STRING) { /* no tape: a scalar, or a string alone (not through the table, as planned) */
		if (UNEXPECTED((size_t) (end - p) != (Z_TYPE_P(x->val) == IS_STRING ? str_size(Z_STRLEN_P(x->val)) : 0))) {
			return false;
		}
		nc_write_flat(e, p, x->val);
	} else {
		writer w = {.cursor = p, .end = end, .tape = x->z.tape, .nt = x->z.nt, .ns = x->z.ns, .narr = x->z.sh ? x->z.sh->narr : 0,
			.str = x->z.str, .g = &x->g, .arr = x->z.sh ? x->z.sh->arr : NULL};
		if (UNEXPECTED(!write_tape(&w, x, v))) {
			return false;
		}
	}
#if NC_CHECKS
	block_check(e);
#endif
	return true;
}

bool nc_write_array(const plan *z, const zend_array *src, void *dst)
{
	writer w = {.cursor = dst, .end = (char *) dst + z->n, .tape = z->tape, .nt = z->nt, .ns = z->ns, .narr = z->sh->narr, .str = z->str, .arr = z->sh->arr};
	if (BAIL_SET(w.bail)) {
		return false;
	}
	zval v;
	ZVAL_ARR(&v, (zend_array *) src);
	write_zval(&w, &v); /* its own tape entry first: a copy */
	zend_array *a = Z_ARR(v);
	NC_ASSERT((void *) a == dst && w.cursor == (char *) dst + z->n && w.ti == z->nt);
	(void) a;
	return true;
}

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
	block_check(e);
#endif
	return r.atoms;
}
