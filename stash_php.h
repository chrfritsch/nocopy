/* The PHP store on top of the arena: stash.c implements it, stash_apcu.c (apcu_*, APCUIterator) uses only this. */

#ifndef STASH_PHP_H
#define STASH_PHP_H

#include "php.h"
#include "php_stash.h"
#include "stash_arena.h"

#define STASH_KEYS 256

/* One value's array keys by hash: dedupes them when persisting, shares their private copies when copying out. */
typedef struct {
	const zend_string *s;
	zend_string *copy;
	zend_ulong h;
	uint32_t gen;
} stash_kslot;

/* STASH_KEYS slots; those of another generation are empty. */
typedef struct {
	stash_kslot *slot; /* own allocation: kept out of the hot globals */
	uint32_t gen;
} stash_keys;

ZEND_BEGIN_MODULE_GLOBALS(stash)
	stash_ctx ctx;
	bool zero_copy; /* this request may be handed pointers into the arena */
	zend_ulong leases[8]; /* key hashes leased: dropped at request end if a callback bailed out */
	int nleases;
	stash_keys keys;
ZEND_END_MODULE_GLOBALS(stash)

ZEND_EXTERN_MODULE_GLOBALS(stash)
#define STASH_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(stash, v)

typedef enum {
	STASH_REFUSE_OBJECTS,    /* objects and resources throw; references are stored as values */
	STASH_SERIALIZE_OBJECTS, /* objects, shared references, deep values: serialize(); resources warn (APCu) */
} stash_objects;

typedef struct {
	zend_long ttl; /* 0: stash.ttl */
	bool add_only; /* fail if a live entry exists */
	stash_objects objects;
} stash_opts;

typedef enum {
	STASH_OK,
	STASH_EXISTS,  /* add_only and a live entry exists */
	STASH_NO_ROOM, /* larger than a shard, or nothing could be evicted */
	STASH_REFUSED, /* the value: warned, or an exception is pending */
} stash_status;

typedef struct {
	time_t created, expires; /* expires 0: never */
	size_t size;             /* bytes in the arena */
	uint64_t seq;            /* insertion order */
} stash_meta;

/* key is borrowed from the arena: copy it to keep it. Must not call back into stash. */
typedef void (*stash_scan_fn)(void *arg, const zend_string *key, const stash_meta *m);

extern bool stash_apc_on;
extern zend_long stash_ini_ttl;

bool stash_tracked(void); /* false (Error thrown): this process/thread has no worker slot */
stash_status stash_store(zend_string *key, zval *val, const stash_opts *o);
bool stash_fetch(zend_string *key, zval *rv);
bool stash_drop(zend_string *key);
bool stash_info(zend_string *key, stash_meta *m); /* m may be NULL: exists */
/* Every live entry, lock-free: may skip or repeat one a concurrent write moves. fn may allocate. */
void stash_scan(stash_scan_fn fn, void *arg);
/* Integers in place under the shard lock. add: creates it as delta (ttl) if missing; false if it is not an integer or there is no room. */
bool stash_add_long(zend_string *key, zend_long delta, zend_long ttl, zend_long *out);
bool stash_cas_long(zend_string *key, zend_long old, zend_long nw);
/* Fetch, or run fci once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void stash_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, const stash_opts *o, double beta, zval *rv);

void stash_apcu_minit(int module_number); /* apcu_* functions, APCUIterator, APC_* constants */

#endif
