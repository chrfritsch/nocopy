/* nocopy: building a write. One walk copies the value into a private buffer, pointers as block offsets; __serialize/__sleep run as met. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"
#include "zend_lazy_objects.h"
#include "zend_enum.h"
#include "zend_exceptions.h"

#ifdef NOCOPY_SHARE_STATS /* bench only (NOCOPY_CFLAGS): how much of what stores hash is shared, to stderr at exit */
static struct { uint64_t arrays, bytes, lookups, hits, hit_arrays, hit_bytes; } sst_put, sst;
static uint64_t sst_puts, sst_by[6]; /* stores by bytes found shared: none hashed, 0%, <25, <50, <75, more */
# define SST(x) (x)
static void sst_end(void)
{
	sst.arrays += sst_put.arrays, sst.bytes += sst_put.bytes, sst.lookups += sst_put.lookups;
	sst.hits += sst_put.hits, sst.hit_arrays += sst_put.hit_arrays, sst.hit_bytes += sst_put.hit_bytes;
	sst_puts++;
	sst_by[!sst_put.bytes ? 0 : !sst_put.hit_bytes ? 1 : MIN(5, 2 + 4 * sst_put.hit_bytes / sst_put.bytes)]++;
}
__attribute__((destructor)) static void sst_print(void)
{
	if (sst_puts) {
		fprintf(stderr, "share\tstores\t%llu\thashed_arrays\t%llu\thashed_bytes\t%llu\tlookups\t%llu\thits\t%llu\thit_arrays\t%llu\thit_bytes\t%llu\t"
			"stores_unhashed\t%llu\tshared_0\t%llu\tlt25\t%llu\tlt50\t%llu\tlt75\t%llu\tge75\t%llu\n",
			(unsigned long long) sst_puts, (unsigned long long) sst.arrays, (unsigned long long) sst.bytes, (unsigned long long) sst.lookups,
			(unsigned long long) sst.hits, (unsigned long long) sst.hit_arrays, (unsigned long long) sst.hit_bytes, (unsigned long long) sst_by[0],
			(unsigned long long) sst_by[1], (unsigned long long) sst_by[2], (unsigned long long) sst_by[3], (unsigned long long) sst_by[4], (unsigned long long) sst_by[5]);
	}
}
#else
# define SST(x) ((void) 0)
#endif

/* ---- the buffer: a block under construction, addressed by block offsets ---- */

static __attribute__((noinline)) void ob_grow(nc_builder *b, size_t n)
{
	size_t cap = b->bcap ? b->bcap : 4096;
	while (cap < b->bn + n) {
		cap *= 2;
	}
	b->buf = perealloc(b->buf, cap, 1);
	b->bcap = cap;
}

/* n bytes at the end: their block offset. */
static zend_always_inline size_t ob_take(nc_builder *b, size_t n)
{
	n = SALIGN(n);
	if (UNEXPECTED(b->bn + n > b->bcap)) {
		ob_grow(b, n);
	}
	size_t r = b->base + b->bn;
	b->bn += n;
	return r;
}

static zend_always_inline void *ob_at(const nc_builder *b, size_t off) { return b->buf + off - b->base; }
#define OFFP(off) ((void *) (uintptr_t) (off))

static __attribute__((noinline)) void rel_grow(nc_builder *b)
{
	b->rcap = b->rcap ? 2 * b->rcap : 1024;
	b->rel = perealloc(b->rel, b->rcap * sizeof *b->rel, 1);
}

/* The pointer at block offset off points into the block. */
static zend_always_inline void rel_add(nc_builder *b, size_t off)
{
	if (UNEXPECTED(b->rn == b->rcap)) {
		rel_grow(b);
	}
	b->rel[b->rn++] = (uint32_t) off;
}

/* A built value into the zval at block offset slot (its u2 kept: a bucket's chain). */
static zend_always_inline void store(nc_builder *b, size_t slot, const zval *v)
{
	zval *d = ob_at(b, slot);
	d->value = v->value;
	d->u1 = v->u1;
	if (nc_off_val(v)) {
		rel_add(b, slot);
	}
}

/* ---- strings ---- */

static __attribute__((noinline)) void str_grow(nc_builder *b)
{
	b->scap *= 2;
	b->str = inl_grow(b->str, b->str_inl, b->ns * sizeof *b->str, b->scap * sizeof *b->str);
}

/* An atom: stored with its hash, refcount 3 above one byte (copy-out shares one copy). First pass only. */
static zend_string *atom_sight(const nocopy_atom_view *v, const zend_string *s, zend_ulong h)
{
	if (!nocopy_atom_admit(h)) {
		return NULL;
	}
	zend_string hdr;
	GC_SET_REFCOUNT(&hdr, ZSTR_LEN(s) > 1 ? 3 : 2);
	GC_TYPE_INFO(&hdr) = arena_str_type(s);
	ZSTR_H(&hdr) = h;
	ZSTR_LEN(&hdr) = ZSTR_LEN(s);
	return nocopy_atom_add(v, h, &hdr, (uint32_t) _ZSTR_HEADER_SIZE, ZSTR_VAL(s), (uint32_t) ZSTR_LEN(s));
}

static zend_string *build_copy(nc_builder *b, const zend_string *s)
{
	size_t size = _ZSTR_STRUCT_SIZE(ZSTR_LEN(s)), off = ob_take(b, size);
	zend_string *r = ob_at(b, off);
	memcpy(r, s, size); /* keys arrive hashed; values as building hashed them, or unhashed (benign idempotent write on use) */
	GC_SET_REFCOUNT(r, 2);
	GC_TYPE_INFO(r) = arena_str_type(s);
	return OFFP(off);
}

/* A key, or a short string inside an array or object: once per value, not at all if it is an atom.
 * The value's table first: the shared one is probed once per distinct string. */
static zend_always_inline zend_string *build_str(nc_builder *b, zend_string *s, zend_ulong h, bool value)
{
	nocopy_strslot *k = strtab_slot(&b->strs, h);
	if (k->gen == b->strs.gen && k->h == h && zend_string_equals(k->s, s)) {
		zend_string *r = b->str[k->ord];
		if (value && ZSTR_LEN(s) > 1 && NC_IS_OFF(r)) {
			GC_SET_REFCOUNT((zend_string *) ob_at(b, (uintptr_t) r), 3); /* used more than once: copy-out shares one copy */
		}
		return r;
	}
	strtab_put(&b->strs, k, s, h);
	zend_string *a = NULL;
	if (b->av && ZSTR_LEN(s) <= NC_VSTR_MAX) {
		a = nocopy_atom_find(b->av, h, ZSTR_VAL(s), (uint32_t) ZSTR_LEN(s));
		if (!a && b->sight) {
			a = atom_sight(b->av, s, h);
		}
	}
	if (UNEXPECTED(b->ns == b->scap)) {
		str_grow(b);
	}
	k->ord = b->ns;
	if (a) {
		b->atoms = true;
	} else {
		a = build_copy(b, s);
	}
	b->str[b->ns++] = a;
	return a;
}

/* A string inside an array or object. */
static zend_always_inline zend_string *build_vstr(nc_builder *b, zend_string *s)
{
	return ZSTR_LEN(s) > NC_VSTR_MAX ? build_copy(b, s) : build_str(b, s, zend_string_hash_val(s), true);
}

/* ---- sharing arrays arena-wide ---- */

/* Hashes for sharing arrays; 0: not shareable (an object, a reference, a long string). Equality is checked, so they only need to
 * spread: an element's hash is independent of the chain, which takes one multiply per element. */
#define H_SEED 0x9E3779B97F4A7C15ULL
static zend_always_inline uint64_t hval(uint64_t type, uint64_t x) { return (x ^ type) * 0xD6E8FEB86659FD93ULL; }
static zend_always_inline uint64_t hchain(uint64_t h, uint64_t key, uint64_t v) { return (h ^ (key * 0x9E3779B97F4A7C15ULL) ^ v) * 0xFF51AFD7ED558CCDULL + (h >> 29); }

static __attribute__((noinline)) void hv_grow(nc_share *sh)
{
	sh->hcap *= 2;
	sh->hv = inl_grow(sh->hv, sh->inl, sh->hn * sizeof *sh->hv, sh->hcap * sizeof *sh->hv);
}

static uint64_t hash_array(nc_share *sh, zend_array *a, int depth, size_t *up);

static zend_always_inline uint64_t hash_zval(nc_share *sh, const zval *v, int depth, size_t *up)
{
	switch (Z_TYPE_P(v)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE:
			return hval(Z_TYPE_P(v), 0);
		case IS_LONG: case IS_DOUBLE: /* the bits: -0.0 is not 0.0 */
			return hval(Z_TYPE_P(v), (uint64_t) Z_LVAL_P(v));
		case IS_STRING:
			*up += str_size(Z_STRLEN_P(v));
			return Z_STRLEN_P(v) <= NC_SHARE_STR ? hval(IS_STRING, zend_string_hash_val(Z_STR_P(v)) ^ Z_STRLEN_P(v)) : 0;
		case IS_ARRAY:
			return hash_array(sh, Z_ARR_P(v), depth, up);
		default:
			return 0;
	}
}

/* The hash walk: a's arrays in building's order (not below objects or references: theirs are regions of their own). */
static uint64_t hash_array(nc_share *sh, zend_array *a, int depth, size_t *up)
{
	if (zend_hash_num_elements(a) == 0) {
		return H_SEED;
	}
	if (depth > NC_MAX_DEPTH) {
		return 0;
	}
	if (UNEXPECTED(sh->hn == sh->hcap)) {
		hv_grow(sh);
	}
	uint32_t i = sh->hn++;
	size_t n = array_bytes(a);
	uint64_t h = H_SEED, c;
	if (HT_IS_PACKED(a)) {
		for (uint32_t k = 0; k < a->nNumUsed; k++) {
			const zval *v = &a->arPacked[k];
			if (Z_TYPE_P(v) != IS_UNDEF) {
				c = hash_zval(sh, v, depth + 1, &n);
				h = h && c ? hchain(h, k, c) : 0;
			}
		}
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key) {
				n += str_size(ZSTR_LEN(b->key));
			}
			c = hash_zval(sh, &b->val, depth + 1, &n);
			h = h && c ? hchain(h, b->key ? ~b->h ^ ZSTR_LEN(b->key) : b->h, c) : 0;
		} ZEND_HASH_FOREACH_END();
	}
	sh->hv[i] = (hnode) {h, sh->hn - i, (uint32_t) MIN(n, UINT32_MAX)};
	*up += n;
	return h;
}

/* Too small to share: under NC_SHARE_MIN bytes with its keys, and only scalars in it. */
static zend_always_inline bool unshareable(const zend_array *a)
{
	size_t n = array_bytes(a);
	if (n >= NC_SHARE_MIN) {
		return false;
	}
	zend_string *k;
	zval *v;
	ZEND_HASH_FOREACH_STR_KEY_VAL(a, k, v) {
		if (Z_TYPE_P(v) >= IS_STRING || (k && (n += str_size(ZSTR_LEN(k))) >= NC_SHARE_MIN)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static void share_reset(nc_share *s)
{
	s->ncand = 0;
	s->hn = s->hc = s->hend = 0;
}

/* The next element of a at *i (holes skipped): its key (NULL: integer h) and value; false past the end. */
static zend_always_inline bool next_elem(const zend_array *a, uint32_t *i, zend_string **k, zend_ulong *h, zval **v)
{
	for (; *i < a->nNumUsed; (*i)++) {
		if (HT_IS_PACKED(a)) {
			*v = &a->arPacked[*i];
			*k = NULL;
			*h = *i;
		} else {
			Bucket *b = &a->arData[*i];
			*v = &b->val;
			*k = b->key;
			*h = b->h;
		}
		if (Z_TYPE_P(*v) != IS_UNDEF) {
			(*i)++;
			return true;
		}
	}
	return false;
}

/* Short strings without a memcmp call. */
static zend_always_inline bool same_str(const zend_string *a, const zend_string *b)
{
	return a == b || (ZSTR_LEN(a) == ZSTR_LEN(b) && nocopy_key_eq(ZSTR_VAL(a), ZSTR_VAL(b), (uint32_t) ZSTR_LEN(a)));
}

static zend_always_inline bool same_val(const zval *va, const zval *vb, int depth)
{
	if (Z_TYPE_P(va) != Z_TYPE_P(vb)) {
		return false;
	}
	switch (Z_TYPE_P(va)) {
		case IS_LONG: case IS_DOUBLE: /* bitwise: -0.0 is not 0.0 */
			return Z_LVAL_P(va) == Z_LVAL_P(vb);
		case IS_STRING:
			return same_str(Z_STR_P(va), Z_STR_P(vb));
		case IS_ARRAY:
			return nc_same_array(Z_ARR_P(va), Z_ARR_P(vb), depth + 1);
		case IS_NULL: case IS_FALSE: case IS_TRUE:
			return true;
		default:
			return false;
	}
}

bool nc_same_array(const zend_array *a, const zend_array *b, int depth)
{
	if (a == b) {
		return true;
	}
	uint32_t n = zend_hash_num_elements(a);
	if (n != zend_hash_num_elements(b) || depth > NC_MAX_DEPTH) {
		return false;
	}
	if (n == a->nNumUsed && n == b->nNumUsed && HT_IS_PACKED(a) == HT_IS_PACKED(b)) { /* no holes: in step */
		if (HT_IS_PACKED(a)) {
			for (uint32_t i = 0; i < n; i++) {
				if (!same_val(&a->arPacked[i], &b->arPacked[i], depth)) {
					return false;
				}
			}
		} else {
			for (uint32_t i = 0; i < n; i++) {
				const Bucket *x = &a->arData[i], *y = &b->arData[i];
				if (x->h != y->h || (x->key == NULL) != (y->key == NULL) || (x->key && !same_str(x->key, y->key))
					|| !same_val(&x->val, &y->val, depth)) {
					return false;
				}
			}
		}
		return true;
	}
	uint32_t i = 0, j = 0;
	zend_string *ka, *kb;
	zend_ulong ha, hb;
	zval *va, *vb;
	while (next_elem(a, &i, &ka, &ha, &va)) {
		if (!next_elem(b, &j, &kb, &hb, &vb) || ha != hb || (ka == NULL) != (kb == NULL) || (ka && !same_str(ka, kb))
			|| !same_val(va, vb, depth)) {
			return false;
		}
	}
	return true;
}

/* The shared array equal to a, else NULL (a may become a candidate). */
static __attribute__((noinline)) zend_array *share_find(nc_builder *b, zend_array *a, const hnode *e)
{
	nc_share *sh = b->sh;
	uint64_t h = e->h;
	SST(sst_put.lookups++);
	zend_array *s = nocopy_atom_find(b->av, h, (const char *) &h, sizeof h | NOCOPY_ATOM_ARRAY);
	if (s && nc_same_array(a, s, 0)) {
		b->atoms = true;
		return s;
	}
	if (!s && e->up <= NC_SHARE_MAX && sh->ncand < NC_SHARE_ADMIT) {
		GROW(sh->cand, sh->ncand, sh->ccap, 8);
		sh->cand[sh->ncand++] = (nc_cand) {a, h};
	}
	return NULL;
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

/* ---- the graph builder ---- */

#define REF_MULTI 0x80000000u

static void graph_b_init_pass(graph_b *g)
{
	pmap_init(&g->objs);
	pmap_init(&g->cls);
	g->nn = g->nh = 0;
	if (g->nref) {
		memset(g->refdone, 0, g->nref);
	}
}

static void graph_b_init(graph_b *g)
{
	g->nref = g->vcap = 0;
	g->refv = NULL;
	g->refdone = NULL;
	graph_b_init_pass(g);
	g->classes = g->cinl;
	g->ccap = sizeof(g->cinl) / sizeof *g->cinl;
	g->nodes = g->inl;
	g->ncap = sizeof(g->inl) / sizeof *g->inl;
	g->hcap = 0;
	g->hooks = NULL;
	g->nres = 0;
	g->rcap = GHOOK_INL;
	g->res = g->rinl;
	g->hk = NULL;
	g->refs_on = false;
	g->on = true;
}

static gnode *graph_b_node(graph_b *g)
{
	if (g->nn == g->ncap) {
		g->ncap *= 4;
		g->nodes = inl_grow(g->nodes, g->inl, sizeof(g->inl), g->ncap * sizeof(gnode));
	}
	gnode *n = &g->nodes[g->nn++];
	ZVAL_UNDEF(&n->data);
	n->o = NULL;
	n->named = NULL;
	n->dyn = n->own = false;
	return n;
}

/* Released between passes: the maps, the nodes' objects and own data, the classes' lowercase names. May run destructors. */
static void graph_b_end_pass(graph_b *g)
{
	pmap_free(&g->objs);
	for (uint32_t i = 0; i < g->cls.n; i++) {
		zend_string_release(g->classes[i].lc);
	}
	pmap_free(&g->cls);
	for (uint32_t i = 0; i < g->nn; i++) {
		if (g->nodes[i].own) {
			zval_ptr_dtor(&g->nodes[i].data);
		}
		if (g->nodes[i].o) {
			OBJ_RELEASE(g->nodes[i].o);
		}
	}
}

void nc_graph_b_free(graph_b *g)
{
	if (!g->on) {
		return;
	}
	graph_b_end_pass(g);
	if (g->classes != g->cinl) {
		efree(g->classes);
	}
	if (g->nodes != g->inl) {
		efree(g->nodes);
	}
	if (g->hooks) {
		efree(g->hooks);
	}
	if (g->refs_on) {
		for (uint32_t i = 0; i <= g->refs.mask; i++) {
			if (g->refs.s[i].k) {
				zval r;
				ZVAL_REF(&r, (zend_reference *) g->refs.s[i].k);
				zval_ptr_dtor(&r);
			}
		}
		pmap_free(&g->refs);
	}
	if (g->refv) {
		efree(g->refv);
		efree(g->refdone);
	}
	for (uint32_t i = 0; i < g->nres; i++) {
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
	g->on = false;
}

/* ce's index << 3 | kind; a class met first is recorded with its lowercase name. */
static uint32_t graph_b_class(graph_b *g, zend_class_entry *ce, nc_kind kind, uint64_t fp)
{
	bool added;
	pslot *s = pmap_add(&g->cls, (uintptr_t) ce, g->cls.n << 3 | kind, &added);
	if (added) {
		uint32_t i = g->cls.n - 1;
		if (i == g->ccap) {
			g->ccap *= 2;
			g->classes = inl_grow(g->classes, g->cinl, sizeof(g->cinl), g->ccap * sizeof(gclass));
		}
		zend_string *lc = zend_string_tolower(ce->name);
		zend_string_hash_val(lc);
		g->classes[i] = (gclass) {.ce = ce, .lc = lc, .fp = fp};
	}
	return s->v;
}

/* o's hook result's index, recorded (not run) the first time. */
static uint32_t graph_b_res(graph_b *g, zend_object *o)
{
	uint32_t i;
	bool added = true;
	if (!g->hk) {
		for (i = 0; i < g->nres; i++) {
			if (g->res[i].o == o) {
				return i;
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
			g->res = inl_grow(g->res, g->rinl, sizeof(g->rinl), g->rcap * sizeof(ghook));
		}
		i = g->nres;
		ghook *h = &g->res[g->nres++];
		GC_ADDREF(o);
		h->o = o;
		ZVAL_UNDEF(&h->data);
		h->named = NULL;
		h->ran = false;
	}
	return i;
}

/* ---- objects and shared references ---- */

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
 * the dynamic properties it names in h->data. BUILD_OBJECT for anything serialize() would warn about: serializing the value repeats the warning. */
static build_status sleep_props(zend_object *o, ghook *h)
{
	zend_class_entry *ce = o->ce;
	zval names;
	if (!nc_call_magic(zend_hash_str_find_ptr(&ce->function_table, ZEND_STRL("__sleep")), o, &names)) {
		return BUILD_ERROR;
	}
	if (Z_TYPE(names) != IS_ARRAY) {
		zval_ptr_dtor(&names);
		return BUILD_OBJECT;
	}
	HashTable *props = zend_std_get_properties(o);
	uint8_t *named = ecalloc(MAX(ce->default_properties_count, 1), 1); /* never NULL: NULL means not called yet */
	HashTable *dyn = NULL;
	build_status st = BUILD_OK;
	zval *nm;
	ZEND_HASH_FOREACH_VAL(Z_ARR(names), nm) {
		ZVAL_DEREF(nm);
		if (Z_TYPE_P(nm) != IS_STRING) {
			st = BUILD_OBJECT;
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
				st = BUILD_OBJECT;
				break;
			}
			named[k] = 1;
		} else if (plain) {
			if (!dyn) {
				dyn = zend_new_array(0);
			}
			if (!zend_hash_add(dyn, name, v)) {
				st = BUILD_OBJECT;
				break;
			}
			Z_TRY_ADDREF_P(v);
		} else {
			st = BUILD_OBJECT;
			break;
		}
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(&names);
	if (st != BUILD_OK) {
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
	return BUILD_OK;
}

static __attribute__((noinline)) void held_add(nc_builder *b, zend_array *a)
{
	GC_ADDREF(a);
	GROW(b->held, b->nheld, b->hcap, 16);
	b->held[b->nheld++] = a;
}

/* User code is about to run: what the walk still reads is held, so writes separate it (COW) and nothing it points to is freed. */
static void hold_walk(nc_builder *b)
{
	for (uint32_t i = 0; i < b->nopen; i++) {
		if (!b->open[i].held && !(GC_FLAGS(b->open[i].a) & GC_IMMUTABLE)) {
			held_add(b, b->open[i].a);
			b->open[i].held = true;
		}
	}
	for (; b->sh && b->cheld < b->sh->ncand; b->cheld++) {
		zend_array *a = b->sh->cand[b->cheld].a;
		if (!(GC_FLAGS(a) & GC_IMMUTABLE)) {
			held_add(b, a);
		}
	}
}

/* Runs res[i]'s __serialize/__sleep (user code). */
static build_status run_hook(nc_builder *b, uint32_t i)
{
	hold_walk(b);
	ghook *h = &b->g->res[i];
	zend_object *o = h->o;
	h->ran = true;
	build_status st = BUILD_OK;
	if (o->ce->__serialize) {
		zval d;
		if (!nc_call_magic(o->ce->__serialize, o, &d)) {
			st = BUILD_ERROR;
		} else if (Z_TYPE(d) != IS_ARRAY) {
			zval_ptr_dtor(&d);
			zend_type_error("%s::__serialize() must return an array", ZSTR_VAL(o->ce->name));
			st = BUILD_ERROR;
		} else {
			ZVAL_COPY_VALUE(&b->g->res[i].data, &d);
		}
	} else {
		st = sleep_props(o, &b->g->res[i]);
	}
	strtab_reset(&b->env->strs); /* it may have stored, and freed strings the table compares against */
	b->strs = b->env->strs;
	return st;
}

/* ---- the walk ---- */

static build_status build_zval(nc_builder *b, zval *v, int depth);
static build_status build_array(nc_builder *b, zend_array *a, int depth, zend_array **out);

static __attribute__((noinline)) build_status build_region(nc_builder *b, zend_array *a, int depth, zend_array **out)
{
	nc_share *sh = b->sh;
	uint32_t hc = sh->hc, hend = sh->hend, base = sh->hn;
	size_t up = 0;
	hash_array(sh, a, depth, &up);
	SST((sst_put.arrays += sh->hn - base, sst_put.bytes += up));
	sh->hc = base;
	sh->hend = sh->hn;
	build_status st = build_array(b, a, depth, out);
	sh->hc = hc;
	sh->hend = hend;
	sh->hn = base;
	return st;
}

/* Where copy_table put a's copy: its header, its data (hash, then elements). */
typedef struct {
	size_t hoff, doff, hsize;
	uint32_t n; /* elements (or buckets) copied */
	bool packed;
} table_at;

/* a's table copied into the buffer, a map's hash shrunk to map_mask and rechained (opcache compacts alike), a sparse one as a map of its elements. */
static zend_always_inline table_at copy_table(nc_builder *b, const zend_array *a)
{
	table_at t;
	t.hoff = ob_take(b, sizeof(zend_array));
	bool sp = sparse(a);
	t.packed = HT_IS_PACKED(a) && !sp;
	uint32_t mask = sp ? sparse_mask(a) : t.packed ? a->nTableMask : map_mask(a);
	uint32_t n = t.n = sp ? a->nNumOfElements : a->nNumUsed;
	size_t hsize = t.hsize = HT_HASH_SIZE(mask);
	t.doff = ob_take(b, hsize + n * (t.packed ? sizeof(zval) : sizeof(Bucket)));
	zend_array *ht = ob_at(b, t.hoff);
	memcpy(ht, a, sizeof(zend_array));
	ht->pDestructor = NULL; /* a function address: differs between processes; immutable arrays are never destroyed */
	ht->nInternalPointer = 0;
	ht->nTableMask = mask;
	ht->nNumUsed = n;
	GC_SET_REFCOUNT(ht, 2);
	GC_TYPE_INFO(ht) = GC_ARRAY | IS_ARRAY_IMMUTABLE;
	char *data = ob_at(b, t.doff);
	if (t.packed) {
		memcpy(data, HT_GET_DATA_ADDR(a), hsize + n * sizeof(zval));
		return t;
	}
	if (sp) {
		ht->nTableSize = pow2_at_least(HT_MIN_SIZE, n);
		HT_FLAGS(ht) &= ~HASH_FLAG_PACKED;
		if (HT_IS_PACKED(a)) {
			HT_FLAGS(ht) |= HASH_FLAG_STATIC_KEYS;
		}
	}
	Bucket *d = (Bucket *) (data + hsize);
	memset(data, 0xff, hsize); /* HT_INVALID_IDX */
	uint32_t i = 0;
	if (sp) {
		zend_ulong k;
		zend_string *key;
		zval *v;
		ZEND_HASH_FOREACH_KEY_VAL(a, k, key, v) {
			d[i++] = (Bucket) {.val = *v, .h = key ? ZSTR_H(key) : k, .key = key};
		} ZEND_HASH_FOREACH_END();
	} else {
		memcpy(d, a->arData, n * sizeof(Bucket));
	}
	for (i = 0; i < n; i++) {
		if (EXPECTED(Z_TYPE(d[i].val) != IS_UNDEF)) {
			uint32_t ni = (uint32_t) d[i].h | mask;
			Z_NEXT(d[i].val) = HT_HASH_EX(d, ni);
			HT_HASH_EX(d, ni) = HT_IDX_TO_HASH(i);
		}
	}
	return t;
}

/* a's copy in the buffer, then each element built: keys through the string table, values by build_zval. */
static build_status build_array(nc_builder *b, zend_array *a, int depth, zend_array **out)
{
	if (zend_hash_num_elements(a) == 0) {
		*out = ARENA_EMPTY;
		return BUILD_OK;
	}
	nc_share *sh = b->sh;
	if (sh) {
		if (sh->hc == sh->hend) {
			if (!unshareable(a)) {
				return build_region(b, a, depth, out);
			}
		} else {
			const hnode *e = &sh->hv[sh->hc++];
			zend_array *s;
			if (e->h && e->up >= NC_SHARE_MIN && (s = share_find(b, a, e))) {
				SST((sst_put.hits++, sst_put.hit_arrays += e->n, sst_put.hit_bytes += e->up));
				sh->hc += e->n - 1; /* its arrays: not built */
				*out = s;
				return BUILD_OK;
			}
		}
	}
	table_at t = copy_table(b, a);
	size_t hoff = t.hoff, doff = t.doff, hsize = t.hsize;
	uint32_t n = t.n;
	bool packed = t.packed;
	/* the copied elements still point into a: it is read until the end, so a hook meanwhile must not free it (hold_walk) */
	NC_ASSERT(b->nopen < NC_OPEN_MAX);
	b->open[b->nopen++] = (nc_open) {a, false};
	/* serialization does not keep the next free index, so neither do we: max int key + 1 */
	zend_long max = ZEND_LONG_MIN;
	size_t step = packed ? sizeof(zval) : sizeof(Bucket), voff = doff + hsize;
	build_status st = BUILD_OK;
	for (uint32_t i = 0; i < n; i++, voff += step) {
		zval v = *(zval *) ob_at(b, voff);
		if (Z_TYPE(v) == IS_UNDEF) {
			continue;
		}
		if (packed) {
			max = (zend_long) i;
		} else {
			Bucket *bk = ob_at(b, voff);
			if (bk->key) {
				zend_string *ks = build_str(b, bk->key, bk->h, false);
				((Bucket *) ob_at(b, voff))->key = ks;
				if (NC_IS_OFF(ks)) {
					rel_add(b, voff + offsetof(Bucket, key));
				}
			} else if ((zend_long) bk->h > max) {
				max = (zend_long) bk->h;
			}
		}
		if (Z_TYPE(v) < IS_STRING) {
			continue; /* copied as it is */
		}
		if ((st = build_zval(b, &v, depth + 1)) != BUILD_OK) {
			break;
		}
		store(b, voff, &v);
	}
	b->nopen--;
	zend_array *ht = ob_at(b, hoff);
	ht->nNextFreeElement = (max == ZEND_LONG_MIN || max == ZEND_LONG_MAX) ? max : max + 1;
	ht->arData = (Bucket *) (uintptr_t) (doff + hsize); /* HT_SET_DATA_ADDR, as an offset */
	rel_add(b, hoff + offsetof(zend_array, arData));
	*out = OFFP(hoff);
	return st;
}

/* The node of o, object i: its slots (or what its kind keeps) after the header. */
static build_status build_node(nc_builder *b, zend_object *o, uint32_t ck, int depth)
{
	graph_b *g = b->g;
	nc_kind kind = (nc_kind) (ck & K_MASK);
	gnode *n = graph_b_node(g);
	uint32_t ni = g->nn - 1;
	n->ck = ck;
	n->o = o;
	GC_ADDREF(o); /* its handle stays unique while user code runs */
	switch (kind) {
		case K_SLEEP: case K_SLEEPW: case K_DATA: {
			uint32_t r = graph_b_res(g, o);
			if (!g->res[r].ran) {
				build_status st = run_hook(b, r);
				if (st != BUILD_OK) {
					return st;
				}
			}
			n = &g->nodes[ni];
			ZVAL_COPY_VALUE(&n->data, &g->res[r].data);
			n->named = g->res[r].named;
			n->dyn = kind_sleep(kind) && Z_TYPE(n->data) == IS_ARRAY;
			break;
		}
		case K_DATE:
			n->own = nc_date_zone_name(o, &n->data);
			break;
		case K_SLOTS: case K_WAKEUP:
			n->dyn = o->properties && has_dynamic(o->properties);
			if (n->dyn && o->ce->default_properties_count) {
				dyn_copy(&n->data, o);
				n->own = true;
			}
			break;
		case K_ENUM:
		case K_NONE:
			break;
	}
	zval data = n->data;
	const uint8_t *named = n->named;
	zend_array *dyn = node_dyn(n, o);
	uint32_t np = kind_nprops(o->ce, kind), nz = np + (dyn != NULL);
	size_t noff = ob_take(b, obj_size(nz)), pz = noff + offsetof(nc_obj, props);
	nc_obj *no = ob_at(b, noff);
	memset(no, 0, obj_size(nz));
	no->cls = ck >> 3;
	no->dyn = dyn != NULL;
	g->nodes[ni].off = noff;
	build_status st = BUILD_OK;
	zval v;
	switch (kind) {
		case K_ENUM:
			ZVAL_STR(&v, Z_STR_P(zend_enum_fetch_case_name(o)));
			Z_STR(v) = build_vstr(b, Z_STR(v));
			Z_TYPE_FLAGS(v) = 0;
			store(b, pz, &v);
			return BUILD_OK;
		case K_DATA:
			v = data;
			if ((st = build_zval(b, &v, depth + 1)) == BUILD_OK) {
				store(b, pz, &v);
			}
			return st;
		case K_DATE: {
			const timelib_time *t = php_date_obj_from_obj(o)->time;
			ZVAL_LONG(&v, (zend_long) t->sse);
			store(b, pz, &v);
			ZVAL_LONG(&v, (zend_long) date_word(t));
			store(b, pz + sizeof(zval), &v);
			v = data;
			if ((st = build_zval(b, &v, depth + 1)) == BUILD_OK) {
				store(b, pz + 2 * sizeof(zval), &v);
			}
			return st;
		}
		case K_SLOTS: case K_WAKEUP: case K_SLEEP: case K_SLEEPW:
			for (uint32_t k = 0; k < np && st == BUILD_OK; k++) {
				if (named && !named[k]) {
					Z_TYPE_INFO(v) = NC_DEFAULT;
				} else {
					v = *OBJ_PROP_NUM(o, k); /* read now: a hook may have changed it */
					st = build_zval(b, &v, depth + 1);
				}
				if (st == BUILD_OK) {
					store(b, pz + k * sizeof(zval), &v);
				}
			}
			if (dyn && st == BUILD_OK) {
				ZVAL_ARR(&v, dyn);
				if ((st = build_zval(b, &v, depth + 1)) == BUILD_OK) {
					store(b, pz + np * sizeof(zval), &v);
				}
			}
			return st;
		case K_NONE:
			break;
	}
	return BUILD_OBJECT;
}

/* *v becomes NC_OREF; met first: its node is built. */
static __attribute__((noinline)) build_status build_object(nc_builder *b, zval *v, int depth)
{
	zend_object *o = Z_OBJ_P(v);
	graph_b *g = b->g;
	if (!g->on) {
		graph_b_init(g);
	}
	bool added;
	uint32_t i = pmap_add(&g->objs, o->handle, g->objs.n, &added)->v;
	if (added) {
		zend_class_entry *ce = o->ce;
		uint64_t fp = nc_class_fp(b->env, ce);
		nc_kind kind = fp_kind(fp);
		if (kind == K_NONE || zend_object_is_lazy(o) || (kind_slots(kind) && o->handlers != &std_object_handlers) || (kind == K_DATE && !nc_date_native(o))) {
			return BUILD_OBJECT;
		}
		build_status st = build_node(b, o, graph_b_class(g, ce, kind, fp), depth);
		if (st != BUILD_OK) {
			return st;
		}
		if (kind_hook(kind)) {
			GROW(g->hooks, g->nh, g->hcap, 16); /* after its contents: inner first */
			g->hooks[g->nh++] = i;
		}
	}
	Z_TYPE_INFO_P(v) = NC_OREF;
	Z_LVAL_P(v) = i;
	b->obj = true;
	return BUILD_OK;
}

static __attribute__((noinline)) void refv_grow(graph_b *g)
{
	g->vcap = g->vcap ? 2 * g->vcap : 8;
	g->refv = erealloc(g->refv, g->vcap * sizeof *g->refv);
	g->refdone = erealloc(g->refdone, g->vcap);
}

/* A reference more than one zval holds: held once in this value, it is built as its value; more often, as a node in the graph's
 * refs. A second sighting of one built as its value starts the walk over (BUILD_AGAIN), knowing it. */
static __attribute__((noinline)) build_status build_ref(nc_builder *b, zval *v, int depth)
{
	graph_b *g = b->g;
	if (!g->on) {
		graph_b_init(g);
	}
	if (!g->refs_on) {
		pmap_init(&g->refs);
		g->refs_on = true;
	}
	zend_reference *r = Z_REF_P(v);
	bool added;
	pslot *s = pmap_add(&g->refs, (uintptr_t) r, b->pass + 1, &added);
	if (added) {
		GC_ADDREF(r); /* its address stays unique while user code runs */
	}
	if (!(s->v & REF_MULTI)) {
		if (!added && s->v == b->pass + 1) {
			if (g->nref == g->vcap) {
				refv_grow(g);
			}
			s->v = REF_MULTI | g->nref++;
			return BUILD_AGAIN;
		}
		s->v = b->pass + 1;
		ZVAL_COPY_VALUE(v, &r->val);
		return build_zval(b, v, depth + 1);
	}
	uint32_t i = s->v & ~REF_MULTI;
	NC_ASSERT(g->refdone && i < g->nref);
	if (!g->refdone[i]) {
		g->refdone[i] = 1;
		zval rv = r->val;
		bool outer = b->obj;
		build_status st = build_zval(b, &rv, depth + 1);
		b->obj = outer;
		if (st != BUILD_OK) {
			return st;
		}
		g->refv[i] = rv;
	}
	Z_TYPE_INFO_P(v) = NC_RREF;
	Z_LVAL_P(v) = i;
	b->obj = true;
	return BUILD_OK;
}

/* Below an object or a reference, arrays start regions of their own: the current one ends here until region_back. */
static zend_always_inline uint32_t region_leave(nc_builder *b)
{
	nc_share *s = b->sh;
	if (!s) {
		return 0;
	}
	uint32_t hend = s->hend;
	s->hend = s->hc;
	return hend;
}

static zend_always_inline build_status region_back(nc_builder *b, uint32_t hend, build_status st)
{
	if (b->sh) {
		b->sh->hend = hend;
	}
	return st;
}

/* Rewrites the copy *v so everything it points to is in the buffer (as offsets), the empty array or an atom. */
static build_status build_zval(nc_builder *b, zval *v, int depth)
{
	if (depth > NC_MAX_DEPTH) {
		return BUILD_DEEP;
	}
	switch (Z_TYPE_P(v)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE:
			return BUILD_OK;
		case IS_STRING:
			Z_STR_P(v) = build_vstr(b, Z_STR_P(v));
			Z_TYPE_FLAGS_P(v) = 0;
			return BUILD_OK;
		case IS_ARRAY: {
			bool outer = b->obj;
			b->obj = false;
			zend_array *r;
			build_status st = build_array(b, Z_ARR_P(v), depth, &r);
			Z_ARR_P(v) = r;
			Z_TYPE_INFO_P(v) = b->obj ? NC_SPINE : IS_ARRAY; /* objects or shared references below: rebuilt on fetch */
			b->obj |= outer;
			return st;
		}
		case IS_REFERENCE: {
			uint32_t hend = region_leave(b);
			if (Z_REFCOUNT_P(v) > 1) {
				return region_back(b, hend, build_ref(b, v, depth));
			}
			ZVAL_COPY_VALUE(v, Z_REFVAL_P(v));
			return region_back(b, hend, build_zval(b, v, depth + 1));
		}
		case IS_RESOURCE:
			return BUILD_RESOURCE;
		case IS_OBJECT: {
			uint32_t hend = region_leave(b);
			return region_back(b, hend, build_object(b, v, depth));
		}
		default:
			return BUILD_OBJECT;
	}
}

/* The graph's header after everything else: counts, classes, shared references' values, object offsets, hooks; the root. */
static void build_graph(nc_builder *b)
{
	graph_b *g = b->g;
	uint32_t ncls = g->cls.n, nobj = g->nn;
	size_t hsz = graph_head(ncls, g->nref, nobj, g->nh), hoff = ob_take(b, hsz);
	nc_graph *gr = ob_at(b, hoff);
	memset(gr, 0, hsz);
	gr->ncls = ncls;
	gr->nobj = nobj;
	gr->nref = g->nref;
	gr->nhooks = g->nh;
	for (uint32_t i = 0; i < nobj; i++) {
		graph_off(gr)[i] = (uint32_t) (int32_t) ((ptrdiff_t) g->nodes[i].off - (ptrdiff_t) hoff);
	}
	if (g->nh) {
		memcpy(graph_hooks(gr), g->hooks, g->nh * sizeof(uint32_t));
	}
	size_t refs = hoff + ((char *) graph_refs(gr) - (char *) gr), cls = hoff + sizeof(nc_graph);
	for (uint32_t i = 0; i < g->nref; i++) {
		store(b, refs + i * sizeof(zval), &g->refv[i]);
	}
	store(b, hoff + offsetof(nc_graph, root), &b->root);
	for (uint32_t i = 0; i < ncls; i++) {
		const gclass *gc = &g->classes[i];
		zend_string *name = build_copy(b, gc->ce->name), *lc = build_copy(b, gc->lc);
		size_t c = cls + i * sizeof(nc_class);
		nc_class *nc = ob_at(b, c);
		nc->name = name;
		nc->lc = lc;
		nc->fp = gc->fp;
		nc->nprops = kind_nprops(gc->ce, fp_kind(gc->fp));
		rel_add(b, c + offsetof(nc_class, name));
		rel_add(b, c + offsetof(nc_class, lc));
	}
	ZVAL_PTR(&b->root, OFFP(hoff));
}

void nc_build_init(nc_builder *b, nocopy_env *env, const nocopy_atom_view *av, nc_share *ss, graph_b *g, zend_string **str, size_t base)
{
	graph_b_off(g);
	b->env = env;
	b->g = g;
	b->av = av;
	b->sight = av != NULL;
	b->sh = av && ss ? nc_share_init(ss) : NULL;
	b->str = b->str_inl = str;
	b->scap = STR_INL;
	b->base = base;
	b->held = NULL;
	b->nheld = b->hcap = 0;
	b->buf = env->buf; /* taken: a hook storing meanwhile gets a buffer of its own */
	b->bcap = env->bcap;
	b->rel = env->rel;
	b->rcap = env->rcap;
	env->buf = NULL;
	env->rel = NULL;
	env->bcap = env->rcap = 0;
}

build_status nc_build(nc_builder *b, zval *val)
{
#ifdef NOCOPY_SHARE_STATS
	bool sst_top = b->sight; /* a store, not an admission */
#endif
	build_status st;
	for (b->pass = 0;; b->pass++) {
		strtab_reset(&b->env->strs);
		b->strs = b->env->strs;
		b->bn = b->rn = 0;
		b->ns = b->nopen = b->cheld = 0;
		b->atoms = b->obj = false;
		SST(memset(&sst_put, 0, sizeof sst_put));
		if (b->sh) {
			share_reset(b->sh);
		}
		if (b->g->on) {
			graph_b_end_pass(b->g);
			graph_b_init_pass(b->g);
		}
		b->root = *val;
		st = build_zval(b, &b->root, 0);
		b->sight = false; /* a later pass meets the same strings again */
		if (st != BUILD_AGAIN) {
			break;
		}
		if (b->pass == NC_MAX_DEPTH) {
			st = BUILD_DEEP;
			break;
		}
	}
	b->fmt = NOCOPY_FMT_PLAIN;
	if (st == BUILD_OK && graph_b_used(b->g)) {
		build_graph(b);
		b->fmt = NOCOPY_FMT_GRAPH;
	}
#ifdef NOCOPY_SHARE_STATS
	if (sst_top) {
		sst_end();
	}
#endif
	return st;
}

void nc_build_bufs_back(nc_builder *b)
{
	nocopy_env *env = b->env;
	if (!b->buf && !b->rel) {
		return;
	}
	if (env->buf) { /* a nested write's: ours is at least as large, as a rule */
		pefree(env->buf, 1);
	}
	if (env->rel) {
		pefree(env->rel, 1);
	}
	env->buf = b->buf;
	env->bcap = b->bcap;
	env->rel = b->rel;
	env->rcap = b->rcap;
	b->buf = NULL;
	b->rel = NULL;
}

void nc_build_free(nc_builder *b)
{
	nc_build_bufs_back(b);
	graph_b_release(b->g);
	for (uint32_t i = 0; i < b->nheld; i++) {
		zend_array_release(b->held[i]);
	}
	if (b->held) {
		efree(b->held);
	}
	if (b->sh) {
		nc_share *s = b->sh;
		if (s->cand) {
			efree(s->cand);
		}
		if (s->hv != s->inl) {
			efree(s->hv);
		}
	}
	if (b->str != b->str_inl) {
		efree(b->str);
	}
}

/* After the commit, still in the view: each candidate the doorkeeper has seen before, built alone and placed into the atoms.
 * Listed outer first; admitted inner first, so outer ones point at them. */
static bool admit_fill(void *dst, void *arg)
{
	nc_place_at(dst, arg);
#if NC_CHECKS
	nc_share_check(dst, arg);
#endif
	return true;
}

void nc_share_admit(nocopy_env *env, const nc_builder *outer, const nocopy_atom_view *av)
{
	for (uint32_t i = outer->sh->ncand; i-- > 0;) {
		const nc_cand *c = &outer->sh->cand[i];
		if (!nocopy_atom_admit(c->h)) {
			continue;
		}
		zend_string *str[STR_INL];
		nc_share ss;
		graph_b g;
		nc_builder b;
		nc_build_init(&b, env, av, &ss, &g, str, 0);
		b.sight = false;
		zval v;
		ZVAL_ARR(&v, c->a);
		/* root not built here: an equal one came in meanwhile and replaced it */
		if (nc_build(&b, &v) == BUILD_OK && b.fmt == NOCOPY_FMT_PLAIN && NC_IS_OFF(Z_ARR(b.root)) && b.bn <= NC_SHARE_MAX) {
			nocopy_atom_add_array(av, c->h, (uint32_t) b.bn, admit_fill, &b, (const char *) &c->h, sizeof c->h);
		}
		nc_build_free(&b);
	}
}
