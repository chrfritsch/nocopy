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

#define ENV_BUF_KEEP (1024 * 1024) /* bytes of write buffer kept past a request: one large write keeps no large buffer */
#define ENV_REL_KEEP (256 * 1024)  /* relocation list entries kept likewise */

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
	if (env->bcap > ENV_BUF_KEEP) {
		pefree(env->buf, 1);
		env->buf = NULL;
		env->bcap = 0;
	}
	if (env->rcap > ENV_REL_KEEP) {
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

/* Arrays and objects see the atoms: a view announced from building until the commit. */
nocopy_status nocopy_value_put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only)
{
	if (Z_TYPE_P(val) <= IS_STRING) {
		return nc_put_flat(ctx, q, val, add_only, NOCOPY_FMT_PLAIN);
	}
	nocopy_atom_view av;
	if ((Z_TYPE_P(val) != IS_ARRAY && Z_TYPE_P(val) != IS_OBJECT) || !nocopy_atom_begin(ctx, &av)) {
		return nc_build_put(env, ctx, q, val, add_only, NULL);
	}
	nocopy_status r = nc_build_put(env, ctx, q, val, add_only, &av);
	nocopy_atom_end(ctx);
	return r;
}

bool nocopy_value_check(nocopy_env *env, zval *val) { return Z_TYPE_P(val) <= IS_STRING || nc_build_check(env, val); }

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
