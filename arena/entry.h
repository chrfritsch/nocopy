/* nocopy arena: an entry is this header, then the caller's block (key bytes at koff, its payload around them). */

#ifndef NOCOPY_ARENA_ENTRY_H
#define NOCOPY_ARENA_ENTRY_H

#include "clock.h"

/* The arena's fields are read-only to its users: they read them through the accessors below and set flags and user through the put request. */
typedef struct nocopy_entry {
	uint64_t h;       /* key hash */
	size_t size;      /* whole block */
	time_t expires;   /* last second it is alive; 0: never */
	uint64_t seq;     /* insertion time, us << 3 | shard; strictly increasing per shard: iterator order without a shared counter */
	uint32_t created; /* unix time: 32 bits keep the header at 48 bytes (good until 2106) */
	uint32_t klen;
	uint8_t koff;     /* key bytes at (char *) e + koff */
	uint8_t live;     /* published and not yet unlinked */
	uint8_t hot;      /* reads since written, log2-ish (policy); halved when merge moves it */
	uint8_t flags;    /* the user's */
	uint32_t user;    /* the user's; kept when merge moves the entry */
} nocopy_entry;

_Static_assert(sizeof(nocopy_entry) == 48, "nocopy_entry grew");

#define NOCOPY_HOT_MAX 15
#define NOCOPY_SEQ_SHARD_BITS 3 /* seq: microseconds << 3 | shard, unique across shards (a segment changes shard, copy mode keys on address + seq) */
_Static_assert(NOCOPY_MAX_SHARDS <= 1 << NOCOPY_SEQ_SHARD_BITS, "shard index must fit seq's low bits");

static inline const char *nocopy_key(const nocopy_entry *e) { return (const char *) e + e->koff; }
static inline uint32_t nocopy_entry_klen(const nocopy_entry *e) { return e->klen; }
static inline size_t nocopy_entry_size(const nocopy_entry *e) { return e->size; }
static inline time_t nocopy_entry_expires(const nocopy_entry *e) { return e->expires; }
static inline time_t nocopy_entry_created(const nocopy_entry *e) { return (time_t) e->created; }
/* One version of a key: grows per shard, and differs for a reused address (copy mode keys kept copies on address + version). */
static inline uint64_t nocopy_entry_version(const nocopy_entry *e) { return e->seq; }
static inline uint8_t nocopy_entry_flags(const nocopy_entry *e) { return e->flags; }
static inline uint32_t nocopy_entry_user(const nocopy_entry *e) { return e->user; }

/* Like APCu: alive through the whole second `expires`, so ttl=1 lives 1-2 s, not 0-1 s. */
static inline bool nocopy_expired(const nocopy_entry *e) { return e->expires && e->expires < nocopy_now(); }
/* An expired entry that a lease waiter may still be served: expired less than its TTL ago. */
static inline bool nocopy_stale_ok(const nocopy_entry *e) { return e->expires && (time_t) (2 * e->expires - e->created) >= nocopy_now(); }

#endif
