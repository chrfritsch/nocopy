/* The PHP store on top of the arena: nocopy.c implements it, nocopy_apcu.c (apcu_*, APCUIterator) uses only this. */

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
} nocopy_local;

/* Valkey connection of this worker (nocopy_valkey.c). */
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
	zend_long ttl; /* 0: nocopy.ttl */
	bool add_only; /* fail if a live entry exists */
	nocopy_objects objects;
} nocopy_opts;

typedef enum {
	NOCOPY_OK,
	NOCOPY_EXISTS,  /* add_only and a live entry exists */
	NOCOPY_NO_ROOM, /* larger than a shard, or nothing could be evicted */
	NOCOPY_REFUSED, /* the value: warned, or an exception is pending */
} nocopy_status;

typedef struct {
	time_t created, expires; /* expires 0: never */
	size_t size;             /* bytes in the arena */
	uint64_t seq;            /* insertion order */
} nocopy_meta;

/* key is borrowed from the arena: copy it to keep it. Must not call back into nocopy. */
typedef void (*nocopy_scan_fn)(void *arg, const zend_string *key, const nocopy_meta *m);

extern bool nocopy_apc_on;
extern zend_long nocopy_ini_ttl;

bool nocopy_tracked(void); /* false (Error thrown): this process/thread has no worker slot */
bool nocopy_available(void); /* false: nocopy.shm_name could not be attached; the functions below then keep and serve nothing */
nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o);
bool nocopy_fetch(zend_string *key, zval *rv);
/* Batches: the local misses in one Valkey round trip, the writes in one pipeline. out: key => value; failed: key => -1 (may be NULL). */
void nocopy_fetch_multi(HashTable *keys, HashTable *out);
bool nocopy_store_multi(HashTable *vals, const nocopy_opts *o, HashTable *failed);
bool nocopy_drop(zend_string *key);
bool nocopy_info(zend_string *key, nocopy_meta *m); /* m may be NULL: exists */
/* Every live entry, lock-free: may skip or repeat one a concurrent write moves. fn may allocate. */
void nocopy_scan(nocopy_scan_fn fn, void *arg);
/* Integers in place under the shard lock. add: creates it as delta (ttl) if missing; false if it is not an integer or there is no room. */
bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool nocopy_cas_long(zend_string *key, zend_long old, zend_long nw);
/* Fetch, or run fci once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void nocopy_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, const nocopy_opts *o, double beta, zval *rv);

void nocopy_clear_all(void); /* apcu_clear_cache: Valkey's keys under the prefix too */
bool nocopy_drop_local(const char *k, size_t len); /* the local copy only */

void nocopy_apcu_minit(int module_number);

/* ---- Valkey tier (nocopy_valkey.c): nocopy.valkey set. Commands take keys without the prefix; -1: unreachable or failed. ---- */
extern bool nocopy_vk_on;
ZEND_INI_MH(nocopy_vk_on_url);
ZEND_INI_DISP(nocopy_vk_display);
ZEND_INI_MH(nocopy_vk_on_path);
ZEND_INI_MH(nocopy_vk_on_verify);
extern char *nocopy_vk_tls_ca, *nocopy_vk_tls_cert, *nocopy_vk_tls_key;
ZEND_INI_MH(nocopy_vk_on_prefix);
ZEND_INI_MH(nocopy_vk_on_timeout);
void nocopy_vk_mshutdown(void);
bool nocopy_vk_ready(void); /* connected and this request's pushes applied; false: serve no local data */
void nocopy_vk_new_request(void);
void nocopy_vk_close(nocopy_vk *v);
void nocopy_vk_minit(bool named); /* after the arena: the anonymous arena's lifeline for its listener */
uint32_t nocopy_vk_listener(void); /* the live listener's pid, 0 none */
int nocopy_vk_mget(uint32_t n, zend_string *const *keys, zend_string **vals); /* one round trip; vals[i]: a copy, NULL if missing */
typedef struct {
	const char *k, *v;
	size_t kl, vl;
} nocopy_vk_kv;
int nocopy_vk_mset(uint32_t n, const nocopy_vk_kv *kv, int64_t pxat, int8_t *res); /* pipelined SETs; res[i] 1 stored, -1 failed */
int nocopy_vk_set(const char *k, size_t kl, const char *val, size_t vl, int64_t pxat, bool nx); /* 0: nx and it exists */
int nocopy_vk_unlink(const char *k, size_t kl); /* 1: it existed */
int nocopy_vk_incr(const char *k, size_t kl, int64_t delta, int64_t ttl_ms, int64_t *out); /* 0: not an integer */
int nocopy_vk_cas(const char *k, size_t kl, int64_t old, int64_t nw);
int nocopy_vk_clear(void); /* apcu_* functions, APCUIterator, APC_* constants */

#endif
