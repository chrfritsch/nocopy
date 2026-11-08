/* The PHP store on top of the arena: module state, the store API (nocopy_store.c) and the Valkey tier's seam (nocopy_valkey.c).
 * Values in and out of blocks: nocopy_value.h. nocopy_* and the module: nocopy.c. */

#ifndef NOCOPY_PHP_H
#define NOCOPY_PHP_H

#include "php.h"
#include "php_nocopy.h"
#include "nocopy_arena.h"
#include "nocopy_value.h"

/* ---- the arena header's user area: the same address in every process that maps it ---- */

typedef struct {
	zend_array empty;	   /* every entry's empty array (unlike zend_empty_array, at one address everywhere) */
	uint32_t empty_hash[2];
	uint64_t inval_gen __attribute__((aligned(64))); /* Valkey: bumped before invalidations are applied (fill guard) */
	int64_t vk_beat;  /* Valkey listener: CLOCK_MONOTONIC ms of its last sign of life, 0 never started */
	uint32_t vk_pid;  /* its pid */
	uint32_t vk_live; /* it is connected and tracking: local copies may be served */
} nocopy_shared;
_Static_assert(sizeof(nocopy_shared) <= sizeof(((nocopy_arena *) 0)->user), "nocopy_shared outgrew the arena's user area");
#define NOCOPY_SHARED ((nocopy_shared *) nocopy_a->user)

/* ---- per process (thread) ---- */

typedef struct nocopy_kept nocopy_kept; /* nocopy_kept.h */

/* Valkey connection of this worker (nocopy_valkey.c; unused without HAVE_NOCOPY_VALKEY). */
typedef struct {
	void *c;		 /* valkeyContext *, NULL: not connected */
	void *ssl;	   /* SSL *, with TLS */
	char err[160];   /* why the connection failed last */
	int64_t retry_at; /* ms, monotonic: after a failure, no new attempt before */
	bool synced;	 /* connected and the listener live, checked in this request */
} nocopy_vk;

ZEND_BEGIN_MODULE_GLOBALS(nocopy)
	nocopy_ctx ctx;
	bool zero_copy; /* this request may be handed pointers into the arena */
	zend_ulong leases[8]; /* key hashes leased: dropped at request end if a callback bailed out */
	int nleases;
	nocopy_keys keys;
	nocopy_kept *kept; /* copy mode's kept copies (nocopy_kept.c): request memory, NULL until the first copy-mode fetch */
	size_t kept_bytes;
	uint32_t kept_hand; /* SIEVE hand over kept */
	bool kept_ok; /* between RINIT and RSHUTDOWN */
	nocopy_vk vk;
	HashTable classes; /* class entry -> layout fingerprint (nocopy_graph.c), this request */
	HashTable zones; /* time zone name -> timelib_tzinfo (ext/date's, freed with it), this request */
ZEND_END_MODULE_GLOBALS(nocopy)

ZEND_EXTERN_MODULE_GLOBALS(nocopy)
#define NOCOPY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(nocopy, v)

/* ---- the store API (nocopy_store.c) ---- */

typedef struct {
	zend_long ttl; /* 0: none */
	time_t expires; /* instead of ttl: the last second it lives (a Valkey fill keeps the remote expiry) */
	bool add_only; /* fail if a live entry exists */
} nocopy_opts;

void nocopy_leases_drop(void); /* leases a bailed-out callback still holds */

bool nocopy_tracked(void); /* false (Error thrown): this process/thread has no worker slot */
nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o);
bool nocopy_fetch(zend_string *key, zval *rv);
/* Batches: the local misses in one Valkey round trip, the writes in one pipeline. out: key => value. */
void nocopy_fetch_multi(HashTable *keys, HashTable *out);
bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o); /* false: a value was not stored */
bool nocopy_drop(zend_string *key);
zend_long nocopy_drop_multi(HashTable *keys); /* how many existed */
/* Integers in place under the shard lock. add: creates it as delta (ttl) if missing; false if it is not an integer or there is no room. */
bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool nocopy_cas_long(zend_string *key, zend_long old, zend_long nw);
/* Fetch, or run fn once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void nocopy_remember(zend_string *key, zval *fn, const nocopy_opts *o, double beta, zval *rv); /* fn: a valid callable */
void nocopy_clear_all(void); /* nocopy_clear: Valkey's keys under the prefix too */

/* The local store alone, for the Valkey tier. */
nocopy_status nocopy_put_local(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user); /* with the compute time; needs an arena */
bool nocopy_fetch_local(zend_string *key, zval *rv, time_t *exp, uint32_t *delta); /* exp/delta may be NULL */
bool nocopy_drop_local(const char *k, size_t len);
bool nocopy_store_each(HashTable *vals, const nocopy_opts *o); /* store_multi as one store per key */
/* A HashTable key as a string (release it). */
static inline zend_string *nocopy_key_of(zend_string *k, zend_ulong h) { return k ? zend_string_copy(k) : zend_long_to_str((zend_long) h); }

/* ---- the detour: where the store ops go off the local fast path. Positional initializers: -Wextra flags a missing slot. ---- */
typedef struct {
	nocopy_status (*store)(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user);
	bool (*fetch)(zend_string *key, zval *rv, time_t *exp, uint32_t *delta);
	void (*fetch_multi)(HashTable *keys, HashTable *out);
	bool (*store_multi)(HashTable *vals, const nocopy_opts *o);
	bool (*drop)(zend_string *key);
	zend_long (*drop_multi)(HashTable *keys);
	bool (*add_long)(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
	bool (*cas_long)(zend_string *key, zend_long old, zend_long nw);
	void (*clear)(void);
	bool leases;	  /* nocopy_remember single-flights through the arena's leases */
	const char *name; /* nocopy_shared_with() */
} nocopy_detour_ops;

extern const nocopy_detour_ops *nocopy_detour; /* NULL: the local store */
extern const nocopy_detour_ops nocopy_no_arena; /* every op misses or fails; stores still check the value */

/* ---- Valkey tier (nocopy_valkey.c): with nocopy.valkey set, the cache of record behind the arena (needs one). ---- */
#ifdef HAVE_NOCOPY_VALKEY
extern bool nocopy_vk_on;
ZEND_INI_MH(nocopy_vk_on_url);
ZEND_INI_DISP(nocopy_vk_display);
ZEND_INI_MH(nocopy_vk_on_path);
ZEND_INI_MH(nocopy_vk_on_verify);
extern char *nocopy_vk_tls_ca, *nocopy_vk_tls_cert, *nocopy_vk_tls_key;
ZEND_INI_MH(nocopy_vk_on_prefix);
ZEND_INI_MH(nocopy_vk_on_timeout);
void nocopy_vk_minit(void);
void nocopy_vk_mshutdown(void);
void nocopy_vk_new_request(void);
void nocopy_vk_close(nocopy_vk *v);
uint32_t nocopy_vk_listener(void); /* the live listener's pid, 0 none */
extern const nocopy_detour_ops nocopy_vk_detour;
#else
/* --disable-nocopy-valkey: never on */
#define nocopy_vk_on false
static inline void nocopy_vk_minit(void) {}
static inline void nocopy_vk_mshutdown(void) {}
static inline void nocopy_vk_new_request(void) {}
static inline void nocopy_vk_close(nocopy_vk *v) { (void) v; }
static inline uint32_t nocopy_vk_listener(void) { return 0; }
#endif

#endif
