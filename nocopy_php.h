/* The PHP store on top of the arena: values (nocopy_value.c, objects: nocopy_graph.c), the store (nocopy_store.c), nocopy_* and the module (nocopy.c). */

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

/* Empties the table for the next value. */
static zend_always_inline void keys_reset(nocopy_keys *t)
{
	if (UNEXPECTED(++t->gen == 0)) {
		memset(t->slot, 0, NOCOPY_KEYS * sizeof(nocopy_kslot));
		t->gen = 1;
	}
}

#define NOCOPY_LOCAL 4096 /* 1024 sets of NOCOPY_LOCAL_WAYS */
#define NOCOPY_LOCAL_WAYS 4

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
	void *c;         /* valkeyContext *, NULL: not connected */
	void *ssl;       /* SSL *, with TLS */
	char err[160];   /* why the connection failed last */
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
	HashTable classes; /* class entry -> layout fingerprint (nocopy_graph.c), this request */
	bool classes_on;
ZEND_END_MODULE_GLOBALS(nocopy)

ZEND_EXTERN_MODULE_GLOBALS(nocopy)
#define NOCOPY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(nocopy, v)

typedef struct {
	zend_long ttl; /* 0: none */
	bool add_only; /* fail if a live entry exists */
} nocopy_opts;

typedef enum {
	NOCOPY_OK,
	NOCOPY_EXISTS,  /* add_only and a live entry exists */
	NOCOPY_NO_ROOM, /* larger than a shard, or nothing could be evicted */
	NOCOPY_REFUSED, /* the value: warned, or an exception is pending */
} nocopy_status;


/* ---- the arena block (nocopy_value.c): core header, the value, then the key's bytes and the value's out-of-line parts ---- */
#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)
typedef struct {
	nocopy_entry h;
	zval val;
} php_entry;
/* nocopy_entry.user: nocopy_remember's compute time, ms (XFetch) */
#define KEY_OFF SALIGN(sizeof(php_entry)) /* the key's bytes come first after the zval: only compared, never a zend_string */
_Static_assert(KEY_OFF <= UINT8_MAX, "koff is 8 bits");
static inline zval *entry_val(nocopy_entry *e) { return &((php_entry *) e)->val; }

/* A HashTable key as a string (release it). */
static inline zend_string *nocopy_key_of(zend_string *k, zend_ulong h) { return k ? zend_string_copy(k) : zend_long_to_str((zend_long) h); }

typedef int (*nocopy_igb_ser_fn)(uint8_t **ret, size_t *ret_len, zval *z);
typedef int (*nocopy_igb_unser_fn)(const uint8_t *buf, size_t buf_len, zval *z);
extern bool nocopy_use_igbinary; /* nocopy.serializer=igbinary, and igbinary resolved (MINIT) */
extern nocopy_igb_ser_fn nocopy_igb_ser;
extern nocopy_igb_unser_fn nocopy_igb_unser;
void nocopy_value_minit(void); /* with an arena: its shared empty array */
void nocopy_copy_out(zval *dst, const zval *src); /* deep private copy of an arena value (NOCOPY_G(keys) shares its deduped keys) */
bool nocopy_graph_out(zval *rv, const zval *v, bool copy); /* an ENTRY_GRAPH value: its objects rebuilt; false: a class is gone or changed */
void nocopy_classes_clear(void); /* at request end */
void *nocopy_graph_tpl(const nocopy_entry *e); /* copy mode: a private template of e's graph, NULL if a class is gone */
bool nocopy_graph_tpl_out(zval *rv, void *tpl); /* false: a hook threw or an enum case is gone */
void nocopy_graph_tpl_free(void *tpl);

/* ---- module state the store reads (set in MINIT / INI) ---- */
extern size_t nocopy_local_size; /* nocopy.local_size */
extern bool nocopy_detour; /* Valkey on, or no arena: operations leave the local fast path */
void nocopy_store_rshutdown(void); /* drops copy mode's copies */
void nocopy_leases_drop(void); /* leases a bailed-out callback still holds */

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
/* Fetch, or run fn once across all workers (lease) and store its result with o. beta > 0: XFetch early refresh. */
void nocopy_remember(zend_string *key, zval *fn, const nocopy_opts *o, double beta, zval *rv); /* fn: a valid callable */

void nocopy_clear_all(void); /* nocopy_clear: Valkey's keys under the prefix too */
bool nocopy_drop_local(const char *k, size_t len); /* the local copy only */

/* ---- the local store's pieces, for the Valkey tier ---- */
#define ENTRY_SERIALIZED 1 /* val is a serialized string; unserialize on fetch */
#define ENTRY_IGBINARY 2   /* with ENTRY_SERIALIZED: written by igbinary */
#define ENTRY_GRAPH 4      /* val is IS_PTR to an nc_graph: plain objects as raw property slots */
/* Sizes and fills the value's entry. NOCOPY_OK: p holds it, ready for nocopy_publish(). p NULL (no arena): only the checks. */
nocopy_status nocopy_build(nocopy_put *p, zend_string *key, zval *val, const nocopy_opts *o);
nocopy_status nocopy_publish(nocopy_put *p, bool add_only);
nocopy_status nocopy_put_local(zend_string *key, zval *val, const nocopy_opts *o, uint32_t user); /* build + publish, with the compute time; needs an arena */
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
