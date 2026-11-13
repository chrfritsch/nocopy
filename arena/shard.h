/* nocopy arena: shards. A key's hash picks its shard; a shard owns, for the arena's life, its lock, its slot table, its log
 * and its policy state, and every write takes only its key's shard lock. Here the parts are composed into writes, deletes,
 * in-place updates, room-making and repair after a dead holder. */

#ifndef NOCOPY_ARENA_SHARD_H
#define NOCOPY_ARENA_SHARD_H

#include "ctx.h"
#include "index.h"
#include "policy.h"

typedef struct {
	ar_lock lock __attribute__((aligned(64))); /* alone on its line */
	uint32_t idx;
	uint64_t last_seq;  /* a global atomic counter here cost 9x at 8 parallel writers */
	ar_policy pol;
	ar_index_hdr ix;
	ar_log log __attribute__((aligned(64)));
} ar_shard;

/* Supplied by the arena's user: e is a byte copy of `from`'s block (a merge moved it); repoint e's pointers into its own block.
 * Returns the deps bits of what the block points to outside itself (nocopy_atom_bits). Shard locked: must not allocate. */
typedef uint8_t (*nocopy_rebase_fn)(nocopy_entry *e, const nocopy_entry *from);

AR_HIDDEN extern ar_shard *ar_shards;
AR_HIDDEN extern uint32_t ar_shard_mask;
AR_HIDDEN extern ar_index ar_ix[NOCOPY_MAX_SHARDS]; /* each shard's table, as this process sees it */

static inline uint32_t ar_shard_idx(uint64_t h) { return ar_part(h, ar_shard_mask); }

/* nshards (a power of two) shards over nsegs segments; zero-filled memory. */
void ar_shards_init(ar_shard *shards, uint32_t nshards, uint32_t slots_per, uint32_t nsegs);
/* tables: nshards * slots_per slots; ghosts: as many uint16 tags. */
void ar_shards_adopt(ar_shard *shards, uint32_t nshards, ar_slot *tables, uint16_t *ghosts, uint32_t slots_per, nocopy_rebase_fn rebase);
/* Any segment depends on bits, after giving back what no longer does: one answer under every shard lock (atoms' rotation). */
bool ar_shards_still_depend(uint8_t bits);
/* Every shard and the pool against their invariants, every shard locked meanwhile. NULL, or the first one broken. For tests. */
const char *ar_shards_verify(void);
void ar_shards_forget_held(void); /* tests: this thread played a lock holder that died */

typedef struct {
	uint64_t used, entries, evacuated, evicted, expired, ghost_hits;
	uint32_t dead_segments;
} ar_shards_stats;
void ar_shards_stat(ar_shards_stats *s); /* lock-free: a snapshot that may be off by concurrent writes */

/* ---- API (nocopy_arena.h) ---- */

typedef enum { NOCOPY_PUT_OK, NOCOPY_PUT_EXISTS, NOCOPY_PUT_FULL } nocopy_put_result;

/* A write: what the arena sets up before the caller fills the block. */
typedef struct {
	uint64_t h;
	const char *key;
	uint32_t klen;
	uint8_t koff;   /* the arena copies the key's bytes here; the rest of the block is the caller's */
	size_t size;    /* the whole block, header included, a multiple of 8 */
	time_t ttl;     /* seconds; 0: never expires */
	time_t expires; /* instead of ttl: the last second it lives */
	uint8_t flags;  /* the user's, as read back by nocopy_entry_flags */
	uint32_t user;  /* the user's, kept when merge moves the entry */
	uint8_t deps;   /* atom halves the payload will reference (nocopy_atom_bit of the writer's view), or 0 */
} nocopy_put_req;

/* A write between nocopy_put_begin and nocopy_put_commit. */
typedef struct {
	ar_shard *sh;
	nocopy_entry *e;
} nocopy_put;

/* Allocates the block, sets its header and key; NULL if it does not fit (larger than a shard's share, or nothing to evict).
 * The shard stays locked: the caller fills its payload (copies only: no allocation, no user code), then commits. */
nocopy_entry *nocopy_put_begin(nocopy_put *p, nocopy_ctx *c, const nocopy_put_req *q);
/* Publishes p->e under its key (add_only: not over a live entry) and unlocks. */
nocopy_put_result nocopy_put_commit(nocopy_put *p, bool add_only);

bool nocopy_delete(uint64_t h, const char *k, uint32_t klen); /* true if it was live */
void ar_shard_unstick(uint32_t i); /* lock-free readers: takes and drops shard i's lock */
#define NOCOPY_GONE (-1)
/* fn(e, arg) on h's live entry under its shard lock; NOCOPY_GONE: none, else fn's result.
 * fn changes the payload in place only, with atomics (readers copy without the lock); no allocation, no arena calls, no user code. */
int nocopy_update(uint64_t h, const char *k, uint32_t klen, int (*fn)(nocopy_entry *e, void *arg), void *arg);
void nocopy_clear(void); /* every shard locked, in index order */

#endif
