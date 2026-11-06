/* The value layer's interface: PHP values in and out of an arena block (nocopy_value.c, objects: nocopy_graph.c). */

#ifndef NOCOPY_VALUE_H
#define NOCOPY_VALUE_H

#include "php.h"
#include "nocopy_arena.h"

/* ---- the key table: one value's equal strings stored once, their private copies shared ---- */

#define NOCOPY_KEYS 256

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

/* ---- the block: core header, the value, then the key's bytes and the value's out-of-line parts ---- */

#define SALIGN(n) ZEND_MM_ALIGNED_SIZE(n)
typedef struct {
	nocopy_entry h;
	zval val;
} php_entry;
/* nocopy_entry.user: nocopy_remember's compute time, ms (XFetch) */
#define KEY_OFF SALIGN(sizeof(php_entry)) /* the key's bytes come first after the zval: only compared, never a zend_string */
_Static_assert(KEY_OFF <= UINT8_MAX, "koff is 8 bits");
static inline zval *entry_val(nocopy_entry *e) { return &((php_entry *) e)->val; }

/* nocopy_entry.flags */
#define ENTRY_SERIALIZED 1 /* val is a serialized string; unserialize on fetch */
#define ENTRY_IGBINARY 2   /* with ENTRY_SERIALIZED: written by igbinary */
#define ENTRY_GRAPH 4	  /* val is IS_PTR to an nc_graph: plain objects as raw property slots */

typedef enum {
	NOCOPY_OK,
	NOCOPY_EXISTS,  /* add_only and a live entry exists */
	NOCOPY_NO_ROOM, /* larger than a shard, or nothing could be evicted */
	NOCOPY_REFUSED, /* the value: warned, or an exception is pending */
} nocopy_status;

/* ---- in ---- */

void nocopy_value_minit(void); /* with an arena: its shared empty array */
/* Sizes the value, then writes and publishes its entry (ttl 0: none; user: nocopy_entry.user).
 * p NULL (no arena): only the checks, so a value a store would refuse is refused alike; then NOCOPY_NO_ROOM. */
nocopy_status nocopy_build(nocopy_put *p, zend_string *key, zval *val, zend_long ttl, uint32_t user, bool add_only);

/* ---- out ---- */

void nocopy_copy_out(zval *dst, const zval *src); /* deep private copy of an arena value (NOCOPY_G(keys) shares its deduped keys) */
bool nocopy_graph_out(zval *rv, const zval *v, bool copy); /* an ENTRY_GRAPH value: its objects rebuilt; false: a class is gone or changed */
void nocopy_classes_clear(void); /* at request end */
void *nocopy_graph_tpl(const nocopy_entry *e); /* copy mode: a private template of e's graph, NULL if a class is gone */
bool nocopy_graph_tpl_out(zval *rv, void *tpl); /* false: a hook threw or an enum case is gone */
void nocopy_graph_tpl_free(void *tpl);

/* ---- serialized values (ENTRY_SERIALIZED, and the Valkey tier's encoding) ---- */

typedef int (*nocopy_igb_ser_fn)(uint8_t **ret, size_t *ret_len, zval *z);
typedef int (*nocopy_igb_unser_fn)(const uint8_t *buf, size_t buf_len, zval *z);
extern bool nocopy_use_igbinary; /* nocopy.serializer=igbinary, and igbinary resolved (MINIT) */
extern nocopy_igb_ser_fn nocopy_igb_ser;
extern nocopy_igb_unser_fn nocopy_igb_unser;
bool nocopy_serialize(zval *val, zval *out, bool *igb);
bool nocopy_unserialize(zval *rv, const char *buf, size_t len, uint8_t flags); /* flags: the entry's (ENTRY_IGBINARY) */

#endif
