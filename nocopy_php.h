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

ZEND_BEGIN_MODULE_GLOBALS(nocopy)
	nocopy_ctx ctx;
	bool zero_copy; /* this request may be handed pointers into the arena */
	zend_ulong leases[8]; /* key hashes leased: dropped at request end if a callback bailed out */
	int nleases;
	nocopy_keys keys;
	nocopy_local *local; /* request memory, NULL until the first copy-mode fetch */
	size_t local_bytes;
	bool local_ok; /* between RINIT and RSHUTDOWN */
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
nocopy_status nocopy_store(zend_string *key, zval *val, const nocopy_opts *o);
bool nocopy_fetch(zend_string *key, zval *rv);
bool nocopy_drop(zend_string *key);
bool nocopy_info(zend_string *key, nocopy_meta *m); /* m may be NULL: exists */
/* Every live entry, lock-free: may skip or repeat one a concurrent write moves. fn may allocate. */
void nocopy_scan(nocopy_scan_fn fn, void *arg);
/* Integers in place under the shard lock. add: creates it as delta (ttl) if missing; false if it is not an integer or there is no room. */
bool nocopy_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool nocopy_cas_long(zend_string *key, zend_long old, zend_long nw);
/* Fetch, or run fci once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void nocopy_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, const nocopy_opts *o, double beta, zval *rv);

void nocopy_apcu_minit(int module_number); /* apcu_* functions, APCUIterator, APC_* constants */

#endif
