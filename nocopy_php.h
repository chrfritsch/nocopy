/* The PHP store on top of the arena: module state, the store API (nocopy_store.c) and the detour (the RESP tier's seam, nocopy_resp.h).
 * Values in and out of blocks: nocopy_value.h. nocopy_* and the module: nocopy.c. */

#ifndef NOCOPY_PHP_H
#define NOCOPY_PHP_H

#include "php.h"
#include "php_nocopy.h"
#include "arena/nocopy_arena.h"
#include "nocopy_value.h"
#include "nocopy_kept.h"
#include "nocopy_resp.h"

/* ---- the arena header's user area: the same address in every process that maps it ---- */

typedef struct {
	nocopy_value_shared value; /* the value layer's: every entry's empty array */
	nocopy_vk_shared vk; /* the RESP tier's */
} nocopy_shared;
_Static_assert(sizeof(nocopy_shared) <= NOCOPY_USER_BYTES, "nocopy_shared outgrew the arena's user area");
#define NOCOPY_SHARED ((nocopy_shared *) nocopy_user_area())

/* ---- per process (thread) ---- */

ZEND_BEGIN_MODULE_GLOBALS(nocopy)
	nocopy_ctx ctx;
	bool zero_copy; /* this request may be handed pointers into the arena */
	zend_ulong leases[8]; /* key hashes leased: dropped at request end if a callback bailed out */
	int nleases;
	nocopy_env env; /* the value layer's */
	nocopy_kept_cache kept; /* copy mode's kept copies */
	nocopy_vk vk; /* the RESP tier's connection */
	const struct nocopy_detour_ops *detour; /* where the store ops go: NULL the local store */
	const char *off; /* why this process (thread) has no cache, NULL: it has one */
ZEND_END_MODULE_GLOBALS(nocopy)

ZEND_EXTERN_MODULE_GLOBALS(nocopy)
#define NOCOPY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(nocopy, v)

/* ---- the store API (nocopy_store.c) ---- */

typedef struct {
	zend_long ttl; /* 0: none */
	time_t expires; /* instead of ttl: the last second it lives (a RESP fill keeps the remote expiry) */
	bool add_only; /* fail if a live entry exists */
} nocopy_opts;

void nocopy_leases_drop(void); /* leases a bailed-out callback still holds */

nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o);
bool nocopy_fetch(zend_string *key, zval *rv);
/* Batches: the local misses in one RESP round trip, the writes in one pipeline. out: key => value. */
void nocopy_fetch_multi(HashTable *keys, HashTable *out);
bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o); /* false: a value was not stored */
bool nocopy_drop(zend_string *key);
zend_long nocopy_drop_multi(HashTable *keys); /* how many existed */
/* Integers in place under the shard lock. add: creates it as delta (ttl) if missing; false if it is not an integer or there is no room. */
bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool nocopy_cas_long(zend_string *key, zend_long old, zend_long nw);
/* Fetch, or run fn once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void nocopy_remember(zend_string *key, zval *fn, const nocopy_opts *o, double beta, zval *rv); /* fn: a valid callable */
bool nocopy_clear_all(void); /* nocopy_clear: RESP's keys under the prefix too; false: not everything was cleared */
void nocopy_list_keys(zend_string *prefix, HashTable *out); /* this host's entries under prefix, oldest first: key => size, created, expires */

/* The local store alone, for the RESP tier. */
nocopy_status nocopy_put_local(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user); /* with the compute time; needs an arena */
bool nocopy_fetch_local(zend_string *key, zval *rv, time_t *exp, uint32_t *delta); /* exp/delta may be NULL */
bool nocopy_drop_local(const char *k, size_t len); /* needs an arena */
bool nocopy_store_each(HashTable *vals, const nocopy_opts *o); /* store_multi as one store per key */
/* Each string of a list of keys as zend_string *key (borrowed); other types are skipped. */
#define NOCOPY_KEYS_FOREACH(keys, key) do { \
	zval *key##_z; \
	ZEND_HASH_FOREACH_VAL(keys, key##_z) { \
		ZVAL_DEREF(key##_z); \
		if (Z_TYPE_P(key##_z) != IS_STRING) { \
			continue; \
		} \
		zend_string *key = Z_STR_P(key##_z);
#define NOCOPY_KEYS_FOREACH_END() } ZEND_HASH_FOREACH_END(); } while (0)
/* A HashTable key as a string (release it). */
static inline zend_string *nocopy_key_of(zend_string *k, zend_ulong h) { return k ? zend_string_copy(k) : zend_long_to_str((zend_long) h); }

/* ---- the detour: where the store ops go off the local fast path. Positional initializers: -Wextra flags a missing slot. ---- */
struct nocopy_detour_ops {
	nocopy_status (*store)(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user);
	bool (*fetch)(zend_string *key, zval *rv, time_t *exp, uint32_t *delta);
	void (*fetch_multi)(HashTable *keys, HashTable *out);
	bool (*store_multi)(HashTable *vals, const nocopy_opts *o);
	bool (*drop)(zend_string *key);
	zend_long (*drop_multi)(HashTable *keys);
	bool (*add_long)(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
	bool (*cas_long)(zend_string *key, zend_long old, zend_long nw);
	bool (*clear)(void);
	bool leases;	  /* nocopy_remember single-flights through the arena's leases */
	const char *name; /* nocopy_shared_with() */
};

extern const nocopy_detour_ops nocopy_no_arena; /* off: every op misses or fails; stores still check the value */

#endif
