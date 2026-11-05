/* The PHP store on top of the arena, implemented in nocopy.c. */

#ifndef NOCOPY_PHP_H
#define NOCOPY_PHP_H

#include "php.h"
#include "php_nocopy.h"
#include "nocopy_arena.h"

#define NOCOPY_KEYS 256

/* One value's array keys by hash: dedupes them when persisting, shares their private copies when copying out. */
typedef struct {
	const zend_string *s;
	zend_string *copy;
	zend_ulong h;
	uint32_t gen;
} nocopy_kslot;

/* NOCOPY_KEYS slots; those of another generation are empty. */
typedef struct {
	nocopy_kslot *slot; /* own allocation: kept out of the hot globals */
	uint32_t gen;
} nocopy_keys;

#define NOCOPY_LOCAL 4096 /* ponytail: direct-mapped, colliding keys evict each other; set-associative if hit rates need it */

/* Copy mode: a private copy of an arena entry, valid while the entry is the same version (address + seq). */
typedef struct {
	const nocopy_entry *e;    /* kept copy's entry, NULL none */
	const nocopy_entry *seen; /* last entry fetched here: kept only when it comes back (one-off fetches never evict) */
	uint64_t seq;
	size_t size;
	zval v;
	bool visited; /* SIEVE: used since the hand last passed */
} nocopy_local;

/* Valkey connection of this worker (nocopy_valkey.c; unused without HAVE_NOCOPY_VALKEY). */
typedef struct {
	int fd;          /* -1: not connected */
	void *ssl;       /* SSL *, with TLS */
	char err[160];   /* why the connection failed last */
	char *buf;       /* replies and pushes, persistent memory */
	size_t len, pos, cap;
	int64_t retry_at; /* ms, monotonic: after a failure, no new attempt before */
	bool synced;     /* connected and the listener live, checked in this request */
} nocopy_vk;

ZEND_BEGIN_MODULE_GLOBALS(nocopy)
	nocopy_ctx ctx;
	bool zero_copy; /* this request may be handed pointers into the arena */
	zend_ulong leases[8]; /* key hashes leased: dropped at request end if a callback bailed out */
	int nleases;
	nocopy_keys keys;
	nocopy_local *local; /* request memory, NULL until the first copy-mode fetch */
	size_t local_bytes;
	uint32_t local_hand; /* SIEVE hand over local */
	bool local_ok; /* between RINIT and RSHUTDOWN */
	nocopy_vk vk;
ZEND_END_MODULE_GLOBALS(nocopy)

ZEND_EXTERN_MODULE_GLOBALS(nocopy)
#define NOCOPY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(nocopy, v)

typedef enum {
	NOCOPY_REFUSE_OBJECTS,    /* objects and resources throw; references are stored as values */
	NOCOPY_SERIALIZE_OBJECTS, /* objects, shared references, deep values: serialize(); resources warn (APCu) */
} nocopy_objects;

typedef struct {
	zend_long ttl; /* 0: none */
	bool add_only; /* fail if a live entry exists */
	nocopy_objects objects;
} nocopy_opts;

typedef enum {
	NOCOPY_OK,
	NOCOPY_EXISTS,  /* add_only and a live entry exists */
	NOCOPY_NO_ROOM, /* larger than a shard, or nothing could be evicted */
	NOCOPY_REFUSED, /* the value: warned, or an exception is pending */
} nocopy_status;


bool nocopy_tracked(void); /* false (Error thrown): this process/thread has no worker slot */
bool nocopy_available(void); /* false: nocopy.shm_name could not be attached; the functions below then keep and serve nothing */
nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o);
bool nocopy_fetch(zend_string *key, zval *rv);
/* Batches: the local misses in one Valkey round trip, the writes in one pipeline. out: key => value; failed: key => -1 (may be NULL). */
void nocopy_fetch_multi(HashTable *keys, HashTable *out);
bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed);
bool nocopy_drop(zend_string *key);
zend_long nocopy_drop_multi(HashTable *keys); /* how many existed */
/* Integers in place under the shard lock. add: creates it as delta (ttl) if missing; false if it is not an integer or there is no room. */
bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool nocopy_cas_long(zend_string *key, zend_long old, zend_long nw);
/* Fetch, or run fci once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void nocopy_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, const nocopy_opts *o, double beta, zval *rv);

void nocopy_clear_all(void); /* nocopy_clear: Valkey's keys under the prefix too */
bool nocopy_drop_local(const char *k, size_t len); /* the local copy only */

/* ---- the local store's pieces, for the Valkey tier ---- */
#define ENTRY_SERIALIZED 1 /* val is a serialized string; unserialize on fetch */
#define ENTRY_IGBINARY 2   /* with ENTRY_SERIALIZED: written by igbinary */
/* Sizes and fills the value's entry. NOCOPY_OK: p holds it, ready for nocopy_publish(). p NULL (no arena): only the checks. */
nocopy_status nocopy_build(nocopy_put *p, zend_string *key, zval *val, const nocopy_opts *o);
nocopy_status nocopy_publish(nocopy_put *p, bool add_only);
bool nocopy_fetch_local(zend_string *key, zval *rv, time_t *exp, uint32_t *delta); /* exp/delta may be NULL */
bool nocopy_serialize(zval *val, zval *out, bool *igb);
bool nocopy_unserialize(zval *rv, const char *buf, size_t len, uint8_t flags);
void nocopy_mark_failed(HashTable *failed, zend_string *key);

/* ---- Valkey tier (nocopy_valkey.c): with nocopy.valkey set, the cache of record behind the arena. Store ops need an arena. ---- */
#ifdef HAVE_NOCOPY_VALKEY
extern bool nocopy_vk_on;
ZEND_INI_MH(nocopy_vk_on_url);
ZEND_INI_DISP(nocopy_vk_display);
ZEND_INI_MH(nocopy_vk_on_path);
ZEND_INI_MH(nocopy_vk_on_verify);
extern char *nocopy_vk_tls_ca, *nocopy_vk_tls_cert, *nocopy_vk_tls_key;
ZEND_INI_MH(nocopy_vk_on_prefix);
ZEND_INI_MH(nocopy_vk_on_timeout);
void nocopy_vk_minit(void); /* after the arena */
void nocopy_vk_mshutdown(void);
void nocopy_vk_new_request(void);
void nocopy_vk_close(nocopy_vk *v);
uint32_t nocopy_vk_listener(void); /* the live listener's pid, 0 none */
nocopy_status nocopy_vk_store(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user);
bool nocopy_vk_fetch(zend_string *key, zval *rv, time_t *exp, uint32_t *delta);
void nocopy_vk_fetch_multi(HashTable *keys, HashTable *out);
bool nocopy_vk_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed);
bool nocopy_vk_drop(zend_string *key);
zend_long nocopy_vk_drop_multi(HashTable *keys);
bool nocopy_vk_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool nocopy_vk_cas_long(zend_string *key, zend_long old, zend_long nw);
void nocopy_vk_clear(void); /* Valkey's keys under the prefix */
#else
/* --disable-nocopy-valkey: never on, so the store ops below are never reached */
#define nocopy_vk_on false
static inline void nocopy_vk_minit(void) {}
static inline void nocopy_vk_mshutdown(void) {}
static inline void nocopy_vk_new_request(void) {}
static inline void nocopy_vk_close(nocopy_vk *v) { (void) v; }
static inline uint32_t nocopy_vk_listener(void) { return 0; }
static inline nocopy_status nocopy_vk_store(zend_string *k, zval *v, const nocopy_opts *o, uint32_t u) { (void) k, (void) v, (void) o, (void) u; return NOCOPY_NO_ROOM; }
static inline bool nocopy_vk_fetch(zend_string *k, zval *rv, time_t *e, uint32_t *d) { (void) k, (void) e, (void) d; ZVAL_NULL(rv); return false; }
static inline void nocopy_vk_fetch_multi(HashTable *k, HashTable *o) { (void) k, (void) o; }
static inline bool nocopy_vk_store_multi(HashTable *v, const nocopy_opts *o, HashTable *f) { (void) v, (void) o, (void) f; return false; }
static inline bool nocopy_vk_drop(zend_string *k) { (void) k; return false; }
static inline zend_long nocopy_vk_drop_multi(HashTable *k) { (void) k; return 0; }
static inline bool nocopy_vk_add_long(zend_string *k, zend_long d, zend_long t, zend_long *o) { (void) k, (void) d, (void) t; *o = 0; return false; }
static inline bool nocopy_vk_cas_long(zend_string *k, zend_long o, zend_long n) { (void) k, (void) o, (void) n; return false; }
static inline void nocopy_vk_clear(void) {}
#endif

#endif
