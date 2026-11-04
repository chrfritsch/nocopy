/* Shared between stash.c (persisting, fetching, stash_*) and stash_apcu.c (apcu_*, APCUIterator). */

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

ZEND_BEGIN_MODULE_GLOBALS(stash)
	stash_ctx ctx;
	bool zero_copy; /* this request may be handed pointers into the arena */
	char *cursor;    /* write position inside the block being filled */
	zend_ulong leases[8]; /* key hashes leased: dropped at request end if a callback bailed out */
	int nleases;
	uint32_t kgen; /* slots of other generations are empty */
	stash_kslot *keys; /* STASH_KEYS, own allocation: kept out of the hot globals */
ZEND_END_MODULE_GLOBALS(stash)

ZEND_EXTERN_MODULE_GLOBALS(stash)
#define STASH_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(stash, v)

/* The arena block: core header, the value, then the key string (at koff) and the value's out-of-line parts. */
typedef struct {
	stash_entry h;
	zval val;
} php_entry;

#define ENTRY_SERIALIZED 1 /* val is a serialized string; unserialize on fetch */

static inline zval *entry_val(stash_entry *e) { return &((php_entry *) e)->val; }
static inline zend_string *entry_key(const stash_entry *e) { return (zend_string *) (stash_key(e) - _ZSTR_HEADER_SIZE); }
static inline stash_shard *key_shard(zend_string *key) { return stash_shard_of(zend_string_hash_val(key)); }

extern bool stash_apc_on;
extern zend_long stash_ini_ttl;

bool stash_tracked(void);
/* Build an invisible entry. NULL: arena full, refused value, or exception pending. *locked: shard still locked (small block). */
stash_entry *stash_build(zend_string *key, zval *val, zend_long ttl, bool apcu_semantics, bool *locked);
stash_put_result stash_publish(stash_entry *e, bool add_only, bool locked); /* consumes e, unlocks */
bool stash_store(zend_string *key, zval *val, zend_long ttl, bool add_only, bool apcu_semantics);
bool stash_fetch(zend_string *key, zval *rv);
bool stash_drop(zend_string *key);
/* Fetch, or run fci once across all workers (lease) and store its result. beta > 0: XFetch early refresh. */
void stash_remember(zend_string *key, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long ttl, double beta, bool apcu, zval *rv);

void stash_apcu_minit(int module_number); /* apcu_* functions, APCUIterator, APC_* constants */

#endif
