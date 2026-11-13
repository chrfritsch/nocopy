/* nocopy: the value layer's entry points. A write is built (user code may run), placed (under the shard lock), then released (destructors). */

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
	env->buf = NULL;
	env->bcap = 0;
	env->rel = NULL;
	env->rcap = 0;
	env_caches_init(env);
}

void nocopy_env_request_end(nocopy_env *env)
{
	zend_hash_destroy(&env->classes);
	zend_hash_destroy(&env->zones);
	env_caches_init(env);
	if (env->bcap > 1024 * 1024) { /* one large write keeps no large buffer */
		pefree(env->buf, 1);
		env->buf = NULL;
		env->bcap = 0;
	}
	if (env->rcap > 256 * 1024) {
		pefree(env->rel, 1);
		env->rel = NULL;
		env->rcap = 0;
	}
}

void nocopy_env_free(nocopy_env *env)
{
	pefree(env->strs.slot, 1);
	if (env->buf) {
		pefree(env->buf, 1);
	}
	if (env->rel) {
		pefree(env->rel, 1);
	}
}

/* ---- in ---- */

static zend_always_inline nocopy_status put_result(nocopy_put_result c)
{
	return c == NOCOPY_PUT_OK ? NOCOPY_OK : c == NOCOPY_PUT_EXISTS ? NOCOPY_EXISTS : NOCOPY_NO_ROOM;
}

/* A scalar or a string alone (a serialized value too): nothing to build or release. */
static zend_always_inline nocopy_status put_flat(nocopy_ctx *ctx, nocopy_put_req *q, const zval *val, bool add_only, nocopy_fmt fmt)
{
	q->size = val_off(q->klen) + (Z_TYPE_P(val) == IS_STRING ? str_size(Z_STRLEN_P(val)) : 0);
	q->koff = KEY_OFF;
	q->flags = (uint8_t) fmt;
	q->deps = 0;
	nocopy_put p;
	nocopy_entry *e = nocopy_put_begin(&p, ctx, q);
	if (!e) {
		return NOCOPY_NO_ROOM;
	}
	nc_write_flat(e, (char *) e + val_off(q->klen), val);
	return put_result(nocopy_put_commit(&p, add_only));
}

/* What building could not take: REFUSED, or the value serialized (NOCOPY_OK, ser to release). */
static nocopy_status not_built(build_status st, zval *val, zval *ser, nocopy_fmt *fmt)
{
	if (st == BUILD_ERROR) {
		return NOCOPY_REFUSED;
	}
	if (st == BUILD_RESOURCE) {
		php_error_docref(NULL, E_WARNING, "nocopy: cannot store resources");
		return NOCOPY_REFUSED;
	}
	return nocopy_serialize(val, ser, fmt) ? NOCOPY_OK : NOCOPY_REFUSED;
}

static nocopy_status put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only, const nocopy_atom_view *av)
{
	zend_string *str[STR_INL];
	nc_share ss;
	graph_b g;
	nc_builder b;
	nc_build_init(&b, env, av, &ss, &g, str, val_off(q->klen));
	build_status st = nc_build(&b, val);
	nocopy_status r;
	if (st == BUILD_OK && b.base + b.bn > UINT32_MAX) {
		nc_build_free(&b); /* offsets and the relocation list are 32-bit */
		return NOCOPY_NO_ROOM;
	}
	if (st == BUILD_OK) {
		q->size = b.base + b.bn;
		q->koff = KEY_OFF;
		q->flags = (uint8_t) b.fmt;
		q->deps = b.atoms && av ? nocopy_atom_bit(av) : 0;
		nocopy_put p;
		nocopy_entry *e = nocopy_put_begin(&p, ctx, q);
		r = NOCOPY_NO_ROOM;
		if (e) {
			nc_place(e, &b);
			r = put_result(nocopy_put_commit(&p, add_only));
		}
		nc_build_bufs_back(&b);
		if (b.sh && b.sh->ncand) {
			nc_share_admit(env, &b, av); /* before the release: candidates may be a node's own data */
		}
		nc_build_free(&b); /* after the commit: may run destructors */
		return r;
	}
	nc_build_free(&b);
	zval ser;
	nocopy_fmt fmt;
	if ((r = not_built(st, val, &ser, &fmt)) != NOCOPY_OK) {
		return r;
	}
	r = put_flat(ctx, q, &ser, add_only, fmt);
	zend_string_release(Z_STR(ser));
	return r;
}

/* Arrays and objects see the atoms: a view announced from building until the commit. */
nocopy_status nocopy_value_put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only)
{
	if (Z_TYPE_P(val) <= IS_STRING) {
		return put_flat(ctx, q, val, add_only, NOCOPY_FMT_PLAIN);
	}
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
	if (Z_TYPE_P(val) <= IS_STRING) {
		return true;
	}
	zend_string *str[STR_INL];
	graph_b g;
	nc_builder b;
	nc_build_init(&b, env, NULL, NULL, &g, str, KEY_OFF);
	build_status st = nc_build(&b, val);
	nc_build_free(&b);
	if (st == BUILD_OK) {
		return true;
	}
	zval ser;
	nocopy_fmt fmt;
	if (not_built(st, val, &ser, &fmt) != NOCOPY_OK) {
		return false;
	}
	zend_string_release(Z_STR(ser));
	return true;
}

/* ---- out ---- */

#if NC_CHECKS
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
#if NC_CHECKS
			NC_ASSERT(no_marker(rv, 0));
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
