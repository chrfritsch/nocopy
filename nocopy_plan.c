/* nocopy: planning a write. Sizes the value and takes every decision writing replays (the tape); runs __serialize/__sleep between passes. */

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

/* ---- the tape and the strings ---- */

static __attribute__((noinline)) void tape_grow(plan *z)
{
	z->tcap *= 2;
	z->tape = inl_grow(z->tape, z->tape_inl, z->nt * sizeof *z->tape, z->tcap * sizeof *z->tape);
}

static zend_always_inline void tape_push(plan *z, uint32_t v)
{
	if (UNEXPECTED(z->nt == z->tcap)) {
		tape_grow(z);
	}
	z->tape[z->nt++] = v;
}

static __attribute__((noinline)) void str_grow(plan *z)
{
	z->scap *= 2;
	z->str = inl_grow(z->str, z->str_inl, z->ns * sizeof *z->str, z->scap * sizeof *z->str);
}

/* An atom: stored with its hash, refcount 3 above one byte (copy-out shares one copy). First pass only. */
static zend_string *atom_sight(const nocopy_atom_view *v, const zend_string *s, zend_ulong h)
{
	if (!nocopy_atom_admit(h)) {
		return NULL;
	}
	zend_string hdr;
	GC_SET_REFCOUNT(&hdr, ZSTR_LEN(s) > 1 ? 3 : 2);
	GC_TYPE_INFO(&hdr) = GC_STRING | IS_STR_INTERNED | (ZSTR_IS_VALID_UTF8(s) ? IS_STR_VALID_UTF8 : 0);
	ZSTR_H(&hdr) = h;
	ZSTR_LEN(&hdr) = ZSTR_LEN(s);
	return nocopy_atom_add(v, h, &hdr, (uint32_t) _ZSTR_HEADER_SIZE, ZSTR_VAL(s), (uint32_t) ZSTR_LEN(s));
}

/* A key, or a short string inside an array or object: stored once per value, not at all if it is an atom.
 * The value's table first: the shared one is probed once per distinct string. */
static zend_always_inline void plan_str(plan *z, zend_string *s, zend_ulong h, nc_opk kind)
{
	nocopy_strslot *k = strtab_slot(&z->strs, h);
	if (k->gen == z->strs.gen && k->h == h && zend_string_equals(k->s, s)) {
		tape_push(z, OP(kind, k->ord << 1));
		return;
	}
	strtab_put(&z->strs, k, s, h);
	zend_string *a = NULL;
	if (z->av && ZSTR_LEN(s) <= NC_VSTR_MAX) {
		a = nocopy_atom_find(z->av, h, ZSTR_VAL(s), (uint32_t) ZSTR_LEN(s));
		if (!a && z->sight) {
			a = atom_sight(z->av, s, h);
		}
	}
	if (UNEXPECTED(z->ns == z->scap)) {
		str_grow(z);
	}
	k->ord = z->ns;
	z->str[z->ns++] = a;
	tape_push(z, OP(kind, k->ord << 1 | T_FIRST));
	if (a) {
		z->atoms = true;
	} else {
		z->n += str_size(ZSTR_LEN(s));
	}
}

/* A string inside an array or object. */
static zend_always_inline void plan_vstr(plan *z, zend_string *s)
{
	if (ZSTR_LEN(s) > NC_VSTR_MAX) {
		tape_push(z, OP_LSTR);
		z->n += str_size(ZSTR_LEN(s));
	} else {
		plan_str(z, s, zend_string_hash_val(s), OP_STR);
	}
}

/* ---- plain values ---- */

/* Hashes for sharing arrays; 0: not shareable (an object, a reference, a long string). Equality is checked, so they only need to
 * spread: an element's hash is independent of the chain, which takes one multiply per element. */
#define H_SEED 0x9E3779B97F4A7C15ULL
static zend_always_inline uint64_t hval(uint64_t type, uint64_t x) { return (x ^ type) * 0xD6E8FEB86659FD93ULL; }
static zend_always_inline uint64_t hchain(uint64_t h, uint64_t key, uint64_t v) { return (h ^ (key * 0x9E3779B97F4A7C15ULL) ^ v) * 0xFF51AFD7ED558CCDULL + (h >> 29); }

static __attribute__((noinline)) void hv_grow(plan *z)
{
	z->sh->hcap *= 2;
	z->sh->hv = inl_grow(z->sh->hv, z->sh->inl, z->sh->hn * sizeof *z->sh->hv, z->sh->hcap * sizeof *z->sh->hv);
}

static uint64_t hash_array(plan *z, zend_array *a, int depth, size_t *up);

static zend_always_inline uint64_t hash_zval(plan *z, const zval *v, int depth, size_t *up)
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
			return hash_array(z, Z_ARR_P(v), depth, up);
		default:
			return 0;
	}
}

/* The hash walk: a's arrays in planning's order (not below objects or references: theirs are regions of their own). */
static uint64_t hash_array(plan *z, zend_array *a, int depth, size_t *up)
{
	if (zend_hash_num_elements(a) == 0) {
		return H_SEED;
	}
	if (depth > NC_MAX_DEPTH) {
		return 0;
	}
	if (UNEXPECTED(z->sh->hn == z->sh->hcap)) {
		hv_grow(z);
	}
	uint32_t i = z->sh->hn++;
	size_t n = SALIGN(sizeof(zend_array)) + SALIGN(data_size(a));
	uint64_t h = H_SEED, c;
	if (HT_IS_PACKED(a)) {
		for (uint32_t k = 0; k < a->nNumUsed; k++) {
			const zval *v = &a->arPacked[k];
			if (Z_TYPE_P(v) != IS_UNDEF) {
				c = hash_zval(z, v, depth + 1, &n);
				h = h && c ? hchain(h, k, c) : 0;
			}
		}
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key) {
				n += str_size(ZSTR_LEN(b->key));
			}
			c = hash_zval(z, &b->val, depth + 1, &n);
			h = h && c ? hchain(h, b->key ? ~b->h ^ ZSTR_LEN(b->key) : b->h, c) : 0;
		} ZEND_HASH_FOREACH_END();
	}
	z->sh->hv[i] = (hnode) {h, z->sh->hn - i, (uint32_t) MIN(n, UINT32_MAX)};
	*up += n;
	return h;
}

/* Too small to share: under NC_SHARE_MIN bytes with its keys, and only scalars in it. */
static zend_always_inline bool unshareable(const zend_array *a)
{
	size_t n = SALIGN(sizeof(zend_array)) + SALIGN(data_size(a));
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

static plan_status plan_zval(plan *z, zval *v, int depth);
static bool share_try(plan *z, zend_array *a, const hnode *e);
static plan_status plan_region(plan *z, zend_array *a, int depth);

static plan_status plan_array(plan *z, zend_array *a, int depth)
{
	if (zend_hash_num_elements(a) == 0) {
		tape_push(z, OP_EMPTY);
		return PLAN_OK;
	}
	if (z->sh) {
		if (z->sh->hc == z->sh->hend) {
			if (unshareable(a)) { /* no region */
				goto plan;
			}
			return plan_region(z, a, depth);
		}
		const hnode *e = &z->sh->hv[z->sh->hc++];
		if (e->h && e->up >= NC_SHARE_MIN && share_try(z, a, e)) {
			SST((sst_put.hits++, sst_put.hit_arrays += e->n, sst_put.hit_bytes += e->up));
			z->sh->hc += e->n - 1; /* its arrays: not planned */
			return PLAN_OK;
		}
	}
plan:
	tape_push(z, OP(OP_ARR, a->nNumUsed & NC_OP_MAX));
	z->n += SALIGN(sizeof(zend_array)) + SALIGN(data_size(a));
	plan_status st;
	if (HT_IS_PACKED(a)) {
		zval *v;
		ZEND_HASH_PACKED_FOREACH_VAL(a, v) {
			if ((st = plan_zval(z, v, depth + 1)) != PLAN_OK) {
				return st;
			}
		} ZEND_HASH_FOREACH_END();
	} else {
		Bucket *b;
		ZEND_HASH_MAP_FOREACH_BUCKET(a, b) {
			if (b->key) {
				plan_str(z, b->key, b->h, OP_KEY);
			}
			if ((st = plan_zval(z, &b->val, depth + 1)) != PLAN_OK) {
				return st;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return PLAN_OK;
}

/* An array outside any region: hashed with its arrays, then planned top-down against those hashes. */
static __attribute__((noinline)) plan_status plan_region(plan *z, zend_array *a, int depth)
{
	uint32_t hc = z->sh->hc, hend = z->sh->hend, base = z->sh->hn;
	size_t up = 0;
	hash_array(z, a, depth, &up);
	SST((sst_put.arrays += z->sh->hn - base, sst_put.bytes += up));
	z->sh->hc = base;
	z->sh->hend = z->sh->hn;
	plan_status st = plan_array(z, a, depth);
	z->sh->hc = hc;
	z->sh->hend = hend;
	z->sh->hn = base;
	return st;
}

static plan_status plan_object(plan *z, zend_object *o, int depth);
static plan_status plan_ref(plan *z, zend_reference *r, int depth);

/* Below an object or a reference, arrays start regions of their own. */
#define OUT_OF_REGION(z, call) do { \
	nc_share *s_ = (z)->sh; \
	uint32_t hend_ = s_ ? s_->hend : 0; \
	if (s_) { s_->hend = s_->hc; } \
	plan_status st_ = (call); \
	if (s_) { s_->hend = hend_; } \
	return st_; \
} while (0)

static plan_status plan_zval(plan *z, zval *v, int depth)
{
	if (depth > NC_MAX_DEPTH) {
		return PLAN_DEEP;
	}
	switch (Z_TYPE_P(v)) {
		case IS_UNDEF: case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE:
			return PLAN_OK;
		case IS_REFERENCE:
			if (Z_REFCOUNT_P(v) > 1) {
				OUT_OF_REGION(z, plan_ref(z, Z_REF_P(v), depth));
			}
			tape_push(z, OP_DEREF);
			OUT_OF_REGION(z, plan_zval(z, Z_REFVAL_P(v), depth + 1));
		case IS_STRING:
			plan_vstr(z, Z_STR_P(v));
			return PLAN_OK;
		case IS_ARRAY:
			return plan_array(z, Z_ARR_P(v), depth);
		case IS_RESOURCE:
			return PLAN_RESOURCE;
		case IS_OBJECT:
			OUT_OF_REGION(z, plan_object(z, Z_OBJ_P(v), depth));
		default:
			return PLAN_OBJECT;
	}
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

/* ---- arrays shared arena-wide ---- */

static void share_reset(nc_share *s)
{
	s->narr = s->ncand = 0;
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

/* An equal shared array replaces a (true: on the tape), else a may become a candidate. */
static __attribute__((noinline)) bool share_try(plan *z, zend_array *a, const hnode *e)
{
	nc_share *sh = z->sh;
	uint64_t h = e->h;
	SST(sst_put.lookups++);
	zend_array *s = nocopy_atom_find(z->av, h, (const char *) &h, sizeof h | NOCOPY_ATOM_ARRAY);
	if (s && nc_same_array(a, s, 0)) {
		if (UNEXPECTED(sh->narr == sh->acap)) {
			sh->acap *= 2;
			sh->arr = inl_grow(sh->arr, sh->arr_inl, sh->narr * sizeof *sh->arr, sh->acap * sizeof *sh->arr);
		}
		sh->arr[sh->narr] = s;
		tape_push(z, OP(OP_SHARED, sh->narr++));
		z->atoms = true;
		return true;
	}
	if (!s && e->up <= NC_SHARE_MAX && sh->ncand < NC_SHARE_ADMIT) {
		GROW(sh->cand, sh->ncand, sh->ccap, 8);
		sh->cand[sh->ncand++] = (nc_cand) {a, h};
	}
	return false;
}

typedef struct {
	const plan *z;
	const zend_array *src;
} admit_arg;

static bool admit_fill(void *dst, void *arg)
{
	const admit_arg *x = arg;
	return nc_write_array(x->z, x->src, dst);
}

/* After the commit, still in the view: each candidate the doorkeeper has seen before, written into the atoms. Listed outer
 * first; admitted inner first, so outer ones point at them. */
void nc_share_admit(nocopy_env *env, const plan *outer, const nocopy_atom_view *av)
{
	for (uint32_t i = outer->sh->ncand; i-- > 0;) {
		const nc_cand *c = &outer->sh->cand[i];
		if (!nocopy_atom_admit(c->h)) {
			continue;
		}
		uint32_t tape[TAPE_INL];
		zend_string *str[STR_INL];
		nc_share ss;
		graph_b g;
		graph_b_off(&g);
		plan z = {.env = env, .strs = env->strs, .g = &g, .av = av, .sh = nc_share_init(&ss), .tape = tape, .tcap = TAPE_INL,
			.tape_inl = tape, .str = str, .scap = STR_INL, .str_inl = str};
		zval v;
		ZVAL_ARR(&v, c->a);
		/* n == 0: an equal one came in meanwhile and replaced it */
		if (nc_plan_walk(&z, &v) == PLAN_OK && z.n && z.n <= NC_SHARE_MAX) {
			admit_arg x = {&z, c->a};
			nocopy_atom_add_array(av, c->h, (uint32_t) z.n, admit_fill, &x, (const char *) &c->h, sizeof c->h);
		}
		nc_plan_free(&z);
	}
}

/* ---- the graph builder ---- */

#define REF_MULTI 0x80000000u

static void graph_b_init_pass(graph_b *g)
{
	pmap_init(&g->objs);
	pmap_init(&g->cls);
	g->refs_on = false;
	g->nref = 0;
	g->nn = g->nh = 0;
}

static void graph_b_init(graph_b *g)
{
	graph_b_init_pass(g);
	g->classes = g->cinl;
	g->ccap = sizeof(g->cinl) / sizeof *g->cinl;
	g->nodes = g->inl;
	g->ncap = sizeof(g->inl) / sizeof *g->inl;
	g->hcap = 0;
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
		g->nodes = inl_grow(g->nodes, g->inl, sizeof(g->inl), g->ncap * sizeof(gnode));
	}
	gnode *n = &g->nodes[g->nn++];
	ZVAL_UNDEF(&n->data);
	n->named = NULL;
	n->dyn = n->done = n->own = false;
	return n;
}

/* Released between passes: the maps, the nodes' own data, the classes' lowercase names. */
static void graph_b_end_pass(graph_b *g)
{
	pmap_free(&g->objs);
	for (uint32_t i = 0; i < g->cls.n; i++) {
		zend_string_release(g->classes[i].lc);
	}
	pmap_free(&g->cls);
	if (g->refs_on) {
		pmap_free(&g->refs);
	}
	for (uint32_t i = 0; i < g->nn; i++) {
		if (g->nodes[i].own) {
			zval_ptr_dtor(&g->nodes[i].data);
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

/* ce's index << 3 | kind; a class met first is recorded with what writing needs. */
static uint32_t graph_b_class(plan *z, zend_class_entry *ce, nc_kind kind, uint64_t fp)
{
	graph_b *g = z->g;
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
		z->n += str_size(ZSTR_LEN(ce->name)) + str_size(ZSTR_LEN(lc));
	}
	return s->v;
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
	return &g->res[i];
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
 * the dynamic properties it names in h->data. PLAN_OBJECT for anything serialize() would warn about: serializing the value repeats the warning. */
static plan_status sleep_props(zend_object *o, ghook *h)
{
	zend_class_entry *ce = o->ce;
	zval names;
	if (!nc_call_magic(zend_hash_str_find_ptr(&ce->function_table, ZEND_STRL("__sleep")), o, &names)) {
		return PLAN_ERROR;
	}
	if (Z_TYPE(names) != IS_ARRAY) {
		zval_ptr_dtor(&names);
		return PLAN_OBJECT;
	}
	HashTable *props = zend_std_get_properties(o);
	uint8_t *named = ecalloc(MAX(ce->default_properties_count, 1), 1); /* never NULL: NULL means not called yet */
	HashTable *dyn = NULL;
	plan_status st = PLAN_OK;
	zval *nm;
	ZEND_HASH_FOREACH_VAL(Z_ARR(names), nm) {
		ZVAL_DEREF(nm);
		if (Z_TYPE_P(nm) != IS_STRING) {
			st = PLAN_OBJECT;
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
				st = PLAN_OBJECT;
				break;
			}
			named[k] = 1;
		} else if (plain) {
			if (!dyn) {
				dyn = zend_new_array(0);
			}
			if (!zend_hash_add(dyn, name, v)) {
				st = PLAN_OBJECT;
				break;
			}
			Z_TRY_ADDREF_P(v);
		} else {
			st = PLAN_OBJECT;
			break;
		}
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(&names);
	if (st != PLAN_OK) {
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
	return PLAN_OK;
}

/* Runs the __serialize/__sleep that planning met but did not run (user code: only between passes). */
static plan_status graph_b_hooks(graph_b *g)
{
	for (; g->nrun < g->nres; g->nrun++) {
		ghook *h = &g->res[g->nrun];
		zend_object *o = h->o;
		h->ran = true;
		if (o->ce->__serialize) {
			zval d;
			if (!nc_call_magic(o->ce->__serialize, o, &d)) {
				return PLAN_ERROR;
			}
			if (Z_TYPE(d) != IS_ARRAY) {
				zval_ptr_dtor(&d);
				zend_type_error("%s::__serialize() must return an array", ZSTR_VAL(o->ce->name));
				return PLAN_ERROR;
			}
			ZVAL_COPY_VALUE(&h->data, &d);
		} else {
			plan_status st = sleep_props(o, h);
			if (st != PLAN_OK) {
				return st;
			}
		}
	}
	return PLAN_OK;
}

/* o's node; false: its __serialize/__sleep has not run yet, so its contents wait for the next pass. */
static bool object_node(graph_b *g, zend_object *o, uint32_t ck, gnode **out)
{
	nc_kind kind = (nc_kind) (ck & K_MASK);
	gnode *n = *out = graph_b_node(g);
	n->ck = ck;
	switch (kind) {
		case K_SLEEP: case K_SLEEPW: case K_DATA: {
			const ghook *h = graph_b_res(g, o);
			ZVAL_COPY_VALUE(&n->data, &h->data);
			n->named = h->named;
			n->dyn = kind_sleep(kind) && Z_TYPE(h->data) == IS_ARRAY;
			return h->ran;
		}
		case K_DATE:
			n->own = nc_date_zone_name(o, &n->data);
			return true;
		case K_SLOTS: case K_WAKEUP:
			n->dyn = o->properties && has_dynamic(o->properties);
			if (n->dyn && o->ce->default_properties_count) {
				dyn_copy(&n->data, o);
				n->own = true;
			}
			return true;
		case K_ENUM:
		case K_NONE:
			return true;
	}
	return true;
}

static plan_status plan_contents(plan *z, zend_object *o, nc_kind kind, zval *data, const uint8_t *named, zend_array *dyn, int depth)
{
	plan_status st = PLAN_OK;
	switch (kind) {
		case K_ENUM:
			plan_vstr(z, Z_STR_P(zend_enum_fetch_case_name(o)));
			return PLAN_OK;
		case K_DATA: case K_DATE:
			return plan_zval(z, data, depth + 1);
		case K_SLOTS: case K_WAKEUP: case K_SLEEP: case K_SLEEPW:
			for (int k = 0; k < o->ce->default_properties_count && st == PLAN_OK; k++) {
				if (!named || named[k]) {
					st = plan_zval(z, OBJ_PROP_NUM(o, k), depth + 1);
				}
			}
			if (dyn && st == PLAN_OK) {
				zval dv;
				ZVAL_ARR(&dv, dyn);
				st = plan_zval(z, &dv, depth + 1);
			}
			return st;
		case K_NONE:
			break;
	}
	return PLAN_OBJECT;
}

static __attribute__((noinline)) plan_status plan_object(plan *z, zend_object *o, int depth)
{
	graph_b *g = z->g;
	if (!g->on) {
		graph_b_init(g);
	}
	bool added;
	uint32_t i = pmap_add(&g->objs, o->handle, g->objs.n, &added)->v;
	tape_push(z, OP(added ? OP_OBJ : OP_OBJ_REF, i));
	if (!added) {
		return PLAN_OK; /* met before: a back reference */
	}
	zend_class_entry *ce = o->ce;
	uint64_t fp = nc_class_fp(z->env, ce);
	nc_kind kind = fp_kind(fp);
	if (kind == K_NONE || zend_object_is_lazy(o) || (kind_slots(kind) && o->handlers != &std_object_handlers) || (kind == K_DATE && !nc_date_native(o))) {
		return PLAN_OBJECT;
	}
	gnode *n;
	bool ready = object_node(g, o, graph_b_class(z, ce, kind, fp), &n);
	/* read before planning the contents: it may move the nodes */
	zval data = n->data;
	const uint8_t *named = n->named;
	zend_array *dyn = node_dyn(n, o);
	z->n += SALIGN(obj_size(kind_nprops(ce, kind) + (dyn != NULL)));
	if (!ready) {
		return PLAN_OK; /* planned in the next pass */
	}
	plan_status st = plan_contents(z, o, kind, &data, named, dyn, depth);
	if (st == PLAN_OK && kind_hook(kind)) {
		GROW(g->hooks, g->nh, g->hcap, 16);
		g->hooks[g->nh++] = i;
	}
	return st;
}

/* A reference more than one zval holds: once in this value, it is written as its value; more often, as a node. */
static __attribute__((noinline)) plan_status plan_ref(plan *z, zend_reference *r, int depth)
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
	pslot *s = pmap_add(&g->refs, (uintptr_t) r, z->nt, &added);
	if (added) {
		tape_push(z, OP_DEREF); /* until a second sighting makes it a node */
		return plan_zval(z, &r->val, depth + 1);
	}
	if (!(s->v & REF_MULTI)) {
		uint32_t idx = g->nref++;
		z->tape[s->v] = OP(OP_REF, idx);
		s->v = REF_MULTI | idx;
	}
	tape_push(z, OP(OP_REF_USE, s->v & ~REF_MULTI));
	return PLAN_OK;
}

/* ---- passes ---- */

plan_status nc_plan_walk(plan *z, zval *val)
{
	strtab_reset(&z->env->strs); /* here, not in the callers: whatever reaches the table starts with an empty one */
	z->strs = z->env->strs;
#ifdef NOCOPY_SHARE_STATS
	bool sst_top = z->sight; /* a store, not an admission */
	memset(&sst_put, 0, sizeof sst_put);
#endif
	plan_status st = plan_zval(z, val, 0);
	z->sight = false; /* a later pass meets the same strings again */
	for (int pass = 0; st == PLAN_OK && z->g->on && z->g->nrun < z->g->nres; pass++) {
		if (pass == NC_MAX_DEPTH || (st = graph_b_hooks(z->g)) != PLAN_OK) {
			st = st == PLAN_OK ? PLAN_DEEP : st;
			break;
		}
		strtab_reset(&z->env->strs); /* a hook may have stored */
		z->strs = z->env->strs;
		z->n = 0;
		z->atoms = false;
		z->nt = z->ns = 0;
		SST(memset(&sst_put, 0, sizeof sst_put));
		if (z->sh) {
			share_reset(z->sh); /* the last pass's arrays may be gone (a node's own data) */
		}
		graph_b_end_pass(z->g);
		graph_b_init_pass(z->g);
		st = plan_zval(z, val, 0);
	}
	if (st == PLAN_OK && graph_b_used(z->g)) {
		z->n += SALIGN(graph_head(z->g->cls.n, z->g->nref, z->g->objs.n, z->g->nh));
	}
#ifdef NOCOPY_SHARE_STATS
	if (sst_top) {
		sst_end();
	}
#endif
	return st;
}

static __attribute__((noinline)) void share_free(nc_share *s)
{
	share_reset(s);
	if (s->arr != s->arr_inl) {
		efree(s->arr);
		s->arr = s->arr_inl;
	}
	if (s->cand) {
		efree(s->cand);
		s->cand = NULL;
	}
	if (s->hv != s->inl) {
		efree(s->hv);
		s->hv = s->inl;
	}
}

void nc_plan_free(plan *z)
{
	if (z->sh) {
		share_free(z->sh);
	}
	if (z->tape != z->tape_inl) {
		efree(z->tape);
		z->tape = z->tape_inl;
	}
	if (z->str != z->str_inl) {
		efree(z->str);
		z->str = z->str_inl;
	}
}
