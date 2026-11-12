/* The value layer: PHP values in and out of arena blocks. Entry points nocopy_value.c; phases nocopy_build.c (one walk into a
 * private buffer, hooks), nocopy_write.c (placing, moving: under a shard lock), nocopy_read.c (copies, graphs, templates); nocopy_class.c, nocopy_serial.c. */

#ifndef NOCOPY_VALUE_H
#define NOCOPY_VALUE_H

#include "php.h"
#include "arena/nocopy_arena.h"

/* ---- the block: the arena's header, the value's zval, the key's bytes, then the value's out-of-line parts ---- */

typedef struct {
	nocopy_entry h;
	zval val;
} nocopy_block;

/* nocopy_entry.flags: how the block holds its value. */
typedef enum {
	NOCOPY_FMT_PLAIN, /* a scalar, a string or arrays of them, immutable: handed out as they are */
	NOCOPY_FMT_GRAPH, /* objects or shared references: val is IS_PTR to the graph, rebuilt on fetch */
	NOCOPY_FMT_PHP,   /* val is serialize()'s string */
	NOCOPY_FMT_IGB,   /* val is igbinary's string */
} nocopy_fmt;

static zend_always_inline nocopy_fmt nocopy_value_fmt(const nocopy_entry *e) { return (nocopy_fmt) nocopy_entry_flags(e); }
static zend_always_inline const zval *nocopy_value_zval(const nocopy_entry *e) { return &((const nocopy_block *) e)->val; }

/* e's integer, to change in place (nocopy_update), or NULL: not an integer. */
static zend_always_inline zval *nocopy_value_long(nocopy_entry *e)
{
	zval *v = &((nocopy_block *) e)->val;
	return nocopy_value_fmt(e) == NOCOPY_FMT_PLAIN && Z_TYPE_P(v) == IS_LONG ? v : NULL;
}

/* The value layer's part of the arena's user area: the same address in every process. */
typedef struct {
	zend_array empty; /* every block's empty array (zend_empty_array lies elsewhere in each binary) */
	uint32_t empty_hash[2];
} nocopy_value_shared;

void nocopy_value_attach(nocopy_value_shared *s); /* once per process, with the arena mapped */

/* ---- a thread's state: the module allocates it, only the value layer reads it ---- */

typedef struct nocopy_strslot nocopy_strslot;

/* One value's equal strings, found by hash: 256 slots, those of another generation empty. */
typedef struct {
	nocopy_strslot *slot;
	uint32_t gen;
} nocopy_strtab;

typedef struct {
	nocopy_strtab strs;
	char *buf; /* a write's block, built before the arena is touched; kept for the next */
	size_t bcap;
	uint32_t *rel; /* its pointer slots, by block offset */
	uint32_t rcap;
	HashTable classes; /* class entry -> fingerprint, this request */
	HashTable zones;   /* time zone name -> timelib_tzinfo (ext/date's), this request */
} nocopy_env;

void nocopy_env_init(nocopy_env *env); /* GINIT */
void nocopy_env_request_end(nocopy_env *env);
void nocopy_env_free(nocopy_env *env); /* GSHUTDOWN */

/* ---- in ---- */

typedef enum {
	NOCOPY_OK,
	NOCOPY_EXISTS,  /* add_only and a live entry exists */
	NOCOPY_NO_ROOM, /* larger than a shard, or nothing could be evicted */
	NOCOPY_REFUSED, /* the value: warned, or an exception is pending */
} nocopy_status;

/* Writes val as q's entry. The caller sets q's h, key, klen, ttl, expires, user; the value sets size, koff, flags, deps.
 * User code (__serialize, __sleep) runs while building, before the arena is touched; destructors after the commit. */
nocopy_status nocopy_value_put(nocopy_env *env, nocopy_ctx *ctx, nocopy_put_req *q, zval *val, bool add_only);
/* The checks alone, without an arena: false if a write would refuse val. */
bool nocopy_value_check(nocopy_env *env, zval *val);

/* The arena's nocopy_rebase_fn: a merge moved e's block from `from`; repoints e's value into it. */
uint8_t nocopy_value_rebase(nocopy_entry *e, const nocopy_entry *from);

/* ---- out ---- */

/* What a read handed out: HIT a value; BORROWED (with or without HIT) pointers into the arena may have escaped, keep e pinned. */
enum { NOCOPY_MISS = 0, NOCOPY_HIT = 1, NOCOPY_BORROWED = 2 };

void nocopy_value_copy(nocopy_env *env, zval *dst, const zval *src); /* a deep private copy of a plain value */
int nocopy_value_read_slow(nocopy_env *env, const nocopy_entry *e, bool copy, zval *rv);

/* e's value into rv if it is a scalar (true): the same in either mode, nothing to copy or pin. The read path tries it first. */
static zend_always_inline bool nocopy_value_scalar(const nocopy_entry *e, zval *rv)
{
	const zval *v = nocopy_value_zval(e);
	if (nocopy_value_fmt(e) != NOCOPY_FMT_PLAIN || Z_TYPE_P(v) >= IS_STRING) {
		return false;
	}
	ZVAL_COPY_VALUE(rv, v);
	return true;
}

/* e's value into rv: the arena's own (zero-copy), or with copy a private one. */
static zend_always_inline int nocopy_value_read(nocopy_env *env, const nocopy_entry *e, bool copy, zval *rv)
{
	const zval *v = nocopy_value_zval(e);
	if (UNEXPECTED(nocopy_value_fmt(e) != NOCOPY_FMT_PLAIN)) {
		return nocopy_value_read_slow(env, e, copy, rv);
	}
	if (Z_TYPE_P(v) < IS_STRING) {
		ZVAL_COPY_VALUE(rv, v);
		return NOCOPY_HIT;
	}
	if (copy) {
		nocopy_value_copy(env, rv, v);
		return NOCOPY_HIT;
	}
	ZVAL_COPY_VALUE(rv, v);
	return NOCOPY_HIT | NOCOPY_BORROWED;
}

/* Copy mode: a graph's private template, its objects rebuilt from it without touching the arena. */
typedef struct nocopy_tpl nocopy_tpl;
nocopy_tpl *nocopy_tpl_make(nocopy_env *env, const nocopy_entry *e); /* NOCOPY_FMT_GRAPH; NULL: a class is gone or changed */
bool nocopy_tpl_read(nocopy_env *env, nocopy_tpl *t, zval *rv);       /* false: a hook threw or an enum case is gone */
void nocopy_tpl_free(nocopy_tpl *t);

/* ---- serialized values (NOCOPY_FMT_PHP/IGB, and the RESP tier's encoding) ---- */

bool nocopy_serial_setup(bool igbinary); /* MINIT: the serializer in effect is igbinary (false: php) */
bool nocopy_serialize(zval *val, zval *out, nocopy_fmt *fmt);
bool nocopy_unserialize(zval *rv, const char *buf, size_t len, nocopy_fmt fmt);

#endif
