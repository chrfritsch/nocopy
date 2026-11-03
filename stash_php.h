/* Shared between stash.c (persisting, fetching, stash_*) and stash_apcu.c (apcu_*, APCUIterator). */

#ifndef STASH_PHP_H
#define STASH_PHP_H

#include "php.h"
#include "php_stash.h"
#include "stash_arena.h"

ZEND_BEGIN_MODULE_GLOBALS(stash)
	stash_ctx ctx;
	int scope_depth; /* >0: this thread may be handed zero-copy pointers */
	char *cursor;    /* write position inside the block being filled */
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

void stash_apcu_minit(zend_class_entry *iterator_ce, int module_number); /* constants, and the APCUIterator class registered by stash.c */

#endif
