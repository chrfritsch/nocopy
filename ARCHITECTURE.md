# Architecture

Single file `stash.c`. Everything below lives in one `mmap(MAP_SHARED|MAP_ANONYMOUS)` created in MINIT, so forked workers share it at the same address (pointers are valid everywhere).

## Layout

`[ header | shards | worker slots | segment table | initial slot tables ][ segment 0 ][ segment 1 ] ...`
- Segment size = power of two so that there are at most 256 segments (min 64 KB). Entries larger than half a segment take a run of contiguous segments.
- Entry = `stash_entry` header + key string + value, all in one block. Strings and arrays are written as opcache-style immutable structures
  (refcount 2, `IS_ARRAY_IMMUTABLE` / interned strings, hash precomputed), so a reader can hand the pointer straight to userland.
- Slot table: open addressing, 64-bit slots = `{ offset/8 (32 bit), hash tag (32 bit) }`; 0 empty, 1 tombstone. A probe compares the tag first and only dereferences on a match.
  `slot_clear` turns a deleted slot into empty when the next slot is empty (no tombstone build-up under churn).
- Each shard has its own table (`stash_table`: mask on its own cache line, then the slots). The first ones sit in the header (`apc.entries_hint` sizes them).
  `grow()` (in `commit`, shard locked, when 3/4 of the slots are non-empty and half are live) doubles it into a run of pool segments (`SEG_TABLE`),
  rehashes the live slots and publishes it with a seq_cst store to `shard->tab`. The old table is frozen from then on and never freed.
  Cost: the shard's writers wait for the rehash (~2.5 ms for 200k entries); readers and other shards don't.

## Shards (MICA-style partitions)

The key hash picks one of up to 8 shards (`shard_of`: Fibonacci mix, because DJB hashes of short keys barely vary in their high bits; 8 shards from ~16 MB, fewer for small arenas).
A shard owns: its lock (test-and-test-and-set, yields after 128 spins, alone on its cache line), its slot table (`tab`, on a read-mostly cache line),
one open segment per TTL bucket, its expiry cursor, its live-byte count, and `used`/`nlive` (non-empty/live slots of its table). Every write takes only its key's shard lock.
A segment belongs to the shard that opened it (`owner`); all entries of a key live in its shard's segments, so a segment's `used/live/pending/max_exp` are only touched under its owner's lock.
The **pool lock** guards the segment pool: FREE -> OPEN/SEALED/CONT (taking a segment, once per segment, not per write) and DEAD -> FREE (`reclaim`).
Lock order: shard, then pool; never the other way. Segment `state` is read with `seg_state()` (atomic) wherever the other lock may be the one held.

## Writes

`build_entry`: size the value (`calc_zval`), lock the shard, `alloc_entry` (bump-allocate in the shard's open segment, or take a fresh one / a run from the pool), copy the value in (`persist_*`),
`commit`: `locate` the slot, publish with a release store, mark the old entry dead, unlock. Blocks up to `STASH_INLINE_PUT` (2 KB) do all of this in one critical section;
larger ones unlock after `alloc_entry`, copy outside the lock and relock in `commit`. `time()` is called inside the lock: measured faster under contention than before it.
Per segment: `used` (bytes appended), `live` (bytes of live entries), `pending` (allocated, unpublished). Dead entries just subtract `live`.
A sealed segment with `live==0 && pending==0` becomes DEAD; `reclaim()` frees DEAD segments that no worker pins.
TTL buckets (Segcache): one open segment per bucket (ttl 0, < 8 s, < 64 s, < 512 s, < 4096 s, < 32768 s, longer), so entries that expire together share segments;
each segment keeps `max_exp`, the latest expiry of its entries (0 in bucket 0 = never). Every write runs `expire_step()` on its shard: a cursor (`exp_next`/`exp_gen`/`exp_off`) walks the entries of
the shard's segments with `max_exp <= now`, at most 64 per write, and unlinks them; after a full pass with nothing to do it idles until the next second. No full slot-table scan.
`exp_gen` is compared with the segment's `gen` (bumped on every free) so a cursor never resumes mid-entry in a reused segment.
When an allocation fails: `reclaim()` -> `expire_step()` with a large budget -> expire other shards (`help_others`, trylock only: we hold our shard)
-> `evacuate()` (copy the shard's sparsest sealed segment, <= 50% live, forward, dropping expired entries, keeping each entry in its TTL bucket; one free segment is kept in reserve)
-> evacuate other shards (trylock) -> give up (cache full).
Expiry is driven by writes per shard: a shard without writes keeps its expired entries (reads already treat them as misses) until some shard needs memory.

## Reads (lock-free) and pins

`find()`: load the shard's table, load slot, compare tag, then `seg_hold(entry)` = publish the entry's segment bit in this worker's bitmap (seq_cst store + fence), re-check the slot is unchanged, only then dereference.
It also re-checks that `shard->tab` is still the table it probed, on every hit: a replaced table's slots may name entries freed since, even in a segment this worker already pins. On a change it retries on the new table.
Bits set during a call are dropped at the end of the call (`borrow_end`), except segments from which a zero-copy pointer escaped (`keep`): those stay until post-deactivate (not RSHUTDOWN: session and other built-ins run their RSHUTDOWN after ours and still serialize user values), `stash_checkpoint()` or the end of `stash_scope()`.
Readers touch only read-mostly header lines: the shard mask is a process-local copy, `shard->tab` and the table mask are alone on their cache lines.
`reclaim()` (pool lock) first collects the DEAD set, then a seq_cst fence, then ORs the bitmaps of all workers up to the high-water mark `nworkers`, and frees DEAD and unpinned segments.
That order matters since shards turn segments DEAD concurrently: reading pins first lets a segment that died afterwards be freed under a reader that pinned it just before (seen as 0x5A reads in the debug build).
A worker whose pid is gone (`kill(pid,0)==ESRCH`) has its bits cleared. Forked children claim a new worker slot (`pthread_atfork`) and inherit the parent's bits.

## Fetch modes

- zero-copy (inside a scope): returns the arena pointer; the segment stays pinned.
- copy (`scope_depth==0`): `copy_out` = `zend_array_dup` + `unshare_contents` (repoint arena strings/keys).
- serialized entries (objects, shared refs): `cache_unserialize` with a private unserialize state.
`stash_scope(fn)` runs `fn` in zero-copy mode and `unshare()`s the result before dropping the pins.

## APCu layer

`apcu_*` functions are thin wrappers over `do_store` / `fetch_value` / `do_delete`; `APCUIterator`, `apcu_cache_info` and `stash_stats` scan the slot table lock-free (`slot_pin`).
`apcu_inc/dec/cas` modify the integer in place under the shard lock with relaxed atomics (readers copy that word). `apcu_clear_cache` takes all shard locks in index order.
INI and constants mirror APCu. `apcu_cache_info` / `apcu_sma_info` report live bytes and free-segment runs.

## Invariants worth remembering

- Never allocate (emalloc, zvals) while holding a shard or the pool lock: an OOM bailout (PHP <= 8.6) or the hard-limit `_exit` (8.7+) would leave it locked for every worker.
- Never wait for a second shard lock while holding one (trylock only); shard before pool.
- Arena strings are `IS_STR_INTERNED` but never `IS_STR_PERMANENT`: that flag promises process lifetime and PCRE then keeps the pointer as a cache key.
- Never dereference an entry before `seg_hold` + re-check (the debug build poisons freed segments with 0x5A to catch this).
- A live entry is always reachable from exactly one slot; `evacuate()` relies on it.
- `seg_check()` is what moves SEALED -> DEAD; call it after any change of `live`/`pending`. DEAD only ends in `reclaim()`.
- Only `grow()` writes `shard->tab`; writers only ever modify the current table. Tables are never freed, so a stale table pointer is always readable.
- Tests that must see pressure use a small `apc.shm_size` (2M-4M, 1-2 shards); tests 004/006/011 depend on that. 015 (16M, 8 shards, 8 processes) is the concurrency stress test:
  run it in a loop on a debug build after touching locking or reclamation, a single pass catches races only sometimes. 018 = readers during table growth.
