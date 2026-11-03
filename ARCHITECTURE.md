# Architecture

Single file `stash.c`. Everything below lives in one `mmap(MAP_SHARED|MAP_ANONYMOUS)` created in MINIT, so forked workers share it at the same address (pointers are valid everywhere).

## Layout

`[ header | worker slots | segment table | slot table ][ segment 0 ][ segment 1 ] ...`
- Segment size = power of two so that there are at most 256 segments (min 64 KB). Entries larger than half a segment take a run of contiguous segments.
- Entry = `stash_entry` header + key string + value, all in one block. Strings and arrays are written as opcache-style immutable structures
  (refcount 2, `IS_ARRAY_IMMUTABLE` / interned+permanent strings, hash precomputed), so a reader can hand the pointer straight to userland.
- Slot table: open addressing, 64-bit slots = `{ offset/8 (32 bit), hash tag (32 bit) }`; 0 empty, 1 tombstone. A probe compares the tag first and only dereferences on a match.
  `slot_clear` turns a deleted slot into empty when the next slot is empty (no tombstone build-up under churn).

## Writes (global spinlock, `arena_lock`: test-and-test-and-set, yields after 128 spins, alone on its cache line)

`build_entry`: size the value (`calc_zval`), lock, `alloc_entry` (bump-allocate in the open segment, or a fresh/run of segments), copy the value in (`persist_*`),
`commit`: `locate` the slot, publish with a release store, mark the old entry dead, unlock. Blocks up to `STASH_INLINE_PUT` (2 KB) do all of this in one critical section;
larger ones unlock after `alloc_entry`, copy outside the lock and relock in `commit`.
Per segment: `used` (bytes appended), `live` (bytes of live entries), `pending` (allocated, unpublished). Dead entries just subtract `live`.
A sealed segment with `live==0 && pending==0` becomes DEAD; `reclaim()` frees DEAD segments that no worker pins.
TTL buckets (Segcache): one open segment per bucket (ttl 0, < 8 s, < 64 s, < 512 s, < 4096 s, < 32768 s, longer), so entries that expire together share segments;
each segment keeps `max_exp`, the latest expiry of its entries (0 in bucket 0 = never). Every write runs `expire_step()`: a cursor (`exp_next`/`exp_off`) walks the entries of
segments with `max_exp <= now`, at most 64 per write, and unlinks them; after a full pass with nothing to do it idles until the next second. There is no full slot-table scan any more.
When an allocation fails: `reclaim()` -> `expire_step()` with a large budget -> `evacuate()` (copy the sparsest sealed segment, <= 50% live, forward, dropping expired entries,
keeping each entry in its original TTL bucket; one free segment is kept in reserve) -> give up (cache full).
`seg_free()` resets `exp_off` when it frees the cursor's segment (a reused segment has different entry boundaries).

## Reads (lock-free) and pins

`find()`: load slot, compare tag, then `seg_hold(entry)` = publish the entry's segment bit in this worker's bitmap (seq_cst store + fence), re-check the slot is unchanged, only then dereference.
Bits set during a call are dropped at the end of the call (`borrow_end`), except segments from which a zero-copy pointer escaped (`keep`): those stay until post-deactivate (not RSHUTDOWN: session and other built-ins run their RSHUTDOWN after ours and still serialize user values), `stash_checkpoint()` or the end of `stash_scope()`.
Readers never touch the arena header: the slot mask is a process-local copy (`slot_mask`).
`reclaim()` ORs the bitmaps of all workers up to the high-water mark `nworkers`; a worker whose pid is gone (`kill(pid,0)==ESRCH`) has its bits cleared.
Forked children claim a new worker slot (`pthread_atfork`) and inherit the parent's bits.

## Fetch modes

- zero-copy (inside a scope): returns the arena pointer; the segment stays pinned.
- copy (`scope_depth==0`): `copy_out` = `zend_array_dup` + `unshare_contents` (repoint arena strings/keys).
- serialized entries (objects, shared refs): `cache_unserialize` with a private unserialize state.
`stash_scope(fn)` runs `fn` in zero-copy mode and `unshare()`s the result before dropping the pins.

## APCu layer

`apcu_*` functions are thin wrappers over `do_store` / `fetch_value` / `do_delete`; `APCUIterator` and `apcu_cache_info` scan the slot table lock-free with pins (`slot_pin`).
`apcu_inc/dec/cas` modify the integer in place under the lock with relaxed atomics (readers copy that word).
INI and constants mirror APCu. `apcu_cache_info` / `apcu_sma_info` report live bytes and free-segment runs.

## Invariants worth remembering

- Never allocate (emalloc, zvals) while holding `arena_lock`: an OOM bailout (PHP <= 8.6) or the hard-limit `_exit` (8.7+) would leave it locked for every worker.
- Arena strings are `IS_STR_INTERNED` but never `IS_STR_PERMANENT`: that flag promises process lifetime and PCRE then keeps the pointer as a cache key.
- Never dereference an entry before `seg_hold` + re-check (the debug build poisons freed segments with 0x5A to catch this).
- A live entry is always reachable from exactly one slot; `evacuate()` relies on it.
- `seg_check()` is what moves SEALED -> DEAD; call it after any change of `live`/`pending`.
- Tests that must see pressure use a small `apc.shm_size` (2M-4M); tests 004/006/011 depend on that.
