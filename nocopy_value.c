/* nocopy: the value layer's entry points. A write is planned (user code may run), written (under the shard lock), then released (destructors). */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "nocopy_value_int.h"

nocopy_value_shared *nc_shared;

/* Built aside and written only where it is missing: zend_empty_array carries this process's addresses, and readers of
 * other processes look at the shared copy at any time. Racing first attaches write the same bytes. */
void nocopy_value_attach(nocopy_value_shared *s)
{
	nocopy_value_shared v;
	memset(&v, 0, sizeof v);
	memcpy(&v.empty, &zend_empty_array, sizeof(zend_array));
	v.empty.pDestructor = NULL;
	v.empty_hash[0] = v.empty_hash[1] = HT_INVALID_IDX;
	HT_SET_DATA_ADDR(&v.empty, s->empty_hash);
	if (memcmp(s, &v, sizeof v) != 0) {
		memcpy(s, &v, sizeof v);
	}
	nc_shared = s;
}

/* ---- a thread's state ---- */

/* zend_hash_init allocates nothing: the first insert does, in the request. */
static void env_caches_init(nocopy_env *env)
{
	zend_hash_init(&env->classes, 16, NULL, NULL, 0);
	zend_hash_init(&env->zones, 8, NULL, NULL, 0);
}

void nocopy_env_init(nocopy_env *env)
{
	env->strs.slot = pecalloc(NC_STRTAB, sizeof(nocopy_strslot), 1);
	env->strs.gen = 0;
	env_caches_init(env);
}

void nocopy_env_request_end(nocopy_env *env)
{
	zend_hash_destroy(&env->classes);
	zend_hash_destroy(&env->zones);
	env_caches_init(env);
}

void nocopy_env_free(nocopy_env *env)
{
	pefree(env->strs.slot, 1);
}

/* ---- in ---- */

/* Plans val into x (OK: release it after the commit); REFUSED leaves nothing to release. tape, str: the plan's first buffers. */
static zend_always_inline nocopy_status encode(encoding *x, nocopy_env *env, zval *val, const nocopy_atom_view *av, uint32_t *tape, zend_string **str)
{
	if (Z_TYPE_P(val) > IS_STRING) { /* a value of its own in the table (a reference too); scalars and strings alone bypass it */
		strtab_reset(&env->strs);
	}
	graph_b_off(&x->g);
	x->z = (plan) {.env = env, .strs = env->strs, .g = &x->g, .av = av, .sight = av != NULL,
		.tape = tape, .tcap = TAPE_INL, .tape_inl = tape, .str = str, .scap = STR_INL, .str_inl = str};
	x->fmt = NOCOPY_FMT_PLAIN;
	x->val = val;
	plan_status st = nc_plan_value(&x->z, val);
	if (st == PLAN_OK && graph_b_used(&x->g)) {
		x->fmt = NOCOPY_FMT_GRAPH;
		return NOCOPY_OK;
	}
	graph_b_release(&x->g); /* nothing to write from it: references held once only, or a value to serialize */
	if (st == PLAN_OK) {
		return NOCOPY_OK;
	}
	nc_plan_free(&x->z);
	if (st == PLAN_ERROR) {
		return NOCOPY_REFUSED;
	}
	if (st == PLAN_RESOURCE) {
		php_error_docref(NULL, E_WARNING, "nocopy: cannot store resources");
		return NOCOPY_REFUSED;
	}
	nocopy_fmt fmt;
	if (!nocopy_serialize(val, &x->ser, &fmt)) {
		return NOCOPY_REFUSED;
	}
	x->fmt = fmt;
	x->val = &x->ser;
	x->z.n = str_size(Z_STRLEN(x->ser)); /* a string alone: no tape, no atoms */
	x->z.nt = x->z.ns = 0;
	x->z.atoms = false;
	return NOCOPY_OK;
}

static void encoding_release(encoding *x)
{
	graph_b_release(&x->g);
	if (x->fmt == NOCOPY_FMT_PHP || x->fmt == NOCOPY_FMT_IGB) {
		zend_string_release(Z_STR(x->ser));
	}
	nc_plan_free(&x->z);
}

static nocopy_status put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only, const nocopy_atom_view *av)
{
	uint32_t tape[TAPE_INL]; /* not zeroed: it cost every store */
	zend_string *str[STR_INL];
	encoding x;
	nocopy_status r = encode(&x, env, val, av, tape, str);
	if (r != NOCOPY_OK) {
		return r;
	}
	q->size = encoding_size(&x, q->klen);
	q->koff = KEY_OFF;
	q->flags = (uint8_t) x.fmt;
	q->deps = x.z.atoms && av ? nocopy_atom_bit(av) : 0;
	nocopy_put p;
	nocopy_entry *e = nocopy_put_begin(&p, ctx, q);
	r = NOCOPY_NO_ROOM;
	if (e) {
		nc_write(&x, e);
		nocopy_put_result c = nocopy_put_commit(&p, add_only);
		r = c == NOCOPY_PUT_OK ? NOCOPY_OK : c == NOCOPY_PUT_EXISTS ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
	}
	encoding_release(&x); /* after the commit: may run destructors */
	return r;
}

/* Arrays and objects see the atoms: a view announced from planning until the commit. Anything else: a tail call. */
nocopy_status nocopy_value_put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only)
{
	nocopy_atom_view av;
	if ((Z_TYPE_P(val) != IS_ARRAY && Z_TYPE_P(val) != IS_OBJECT) || !nocopy_atom_begin(ctx, &av)) {
		return put(env, ctx, q, val, add_only, NULL);
	}
	nocopy_status r = put(env, ctx, q, val, add_only, &av);
	nocopy_atom_end(ctx);
	return r;
}

bool nocopy_value_check(nocopy_env *env, zval *val)
{
	uint32_t tape[TAPE_INL];
	zend_string *str[STR_INL];
	encoding x;
	if (encode(&x, env, val, NULL, tape, str) != NOCOPY_OK) {
		return false;
	}
	encoding_release(&x);
	return true;
}

/* ---- out ---- */

#if ZEND_DEBUG
/* No graph marker reached userland (objects' properties are not followed: cycles). */
static bool no_marker(const zval *v, int depth)
{
	ZVAL_DEREF(v);
	if (Z_TYPE_P(v) > IS_PTR) {
		return false;
	}
	if (Z_TYPE_P(v) == IS_ARRAY && depth < NC_MAX_DEPTH) {
		zval *e;
		ZEND_HASH_FOREACH_VAL(Z_ARR_P(v), e) {
			if (!no_marker(e, depth + 1)) {
				return false;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return true;
}
#endif

int nocopy_value_read_slow(nocopy_env *env, const nocopy_entry *e, bool copy, zval *rv)
{
	const zval *v = nocopy_value_zval(e);
	nocopy_fmt fmt = nocopy_value_fmt(e);
	switch (fmt) {
		case NOCOPY_FMT_GRAPH: {
			int r = nc_graph_out(env, rv, v, copy) ? NOCOPY_HIT : NOCOPY_MISS;
#if ZEND_DEBUG
			ZEND_ASSERT(no_marker(rv, 0));
#endif
			return copy ? r : r | NOCOPY_BORROWED; /* a hook may have kept an object even when the build failed */
		}
		case NOCOPY_FMT_PHP:
		case NOCOPY_FMT_IGB:
			return nocopy_unserialize(rv, Z_STRVAL_P(v), Z_STRLEN_P(v), fmt) ? NOCOPY_HIT : NOCOPY_MISS;
		case NOCOPY_FMT_PLAIN:
			break;
	}
	return nocopy_value_read(env, e, copy, rv);
}
