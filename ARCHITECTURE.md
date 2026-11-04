Layers, each using only the one below it:
- Arena (no PHP): entries are a header plus the caller's bytes. `stash_arena.h` is its public API; the read path (`stash_find`, pins, the lock fast path) is inline there. `stash_arena_int.h` is shared by its units only:
  `stash_arena.c` (setup, locks and `shard_repair`, worker slots and pins, segments, slot tables, writes, scans, stats), `stash_evict.c` (expiry, merge, ghost), `stash_lease.c` (single-flight leases), `stash_owner.c` (owner ids: pid + start time).
- `stash.c`: PHP values in and out of the arena (size, persist, rebase, copy-out, unserialize), the store API in `stash_php.h` (`stash_store`/`fetch`/`drop`/`info`/`scan`/`add_long`/`cas_long`/`remember`), `stash_*`, the module, FrankenPHP's worker hook.
- `stash_apcu.c`: `apcu_*` and `APCUIterator`, only through `stash_php.h` (no locks, no arena fields); registered only with `stash.apcu_compat=1`.

`tests/c/stress.c` drives the arena directly from threads (plain and ThreadSanitizer, `tests/c/run.sh`); `tests/c/repair.c` uses the internal header. Both supply `stash_rebase()`, the one callback the arena needs from its user (see Merge).

Everything lives in one `mmap(MAP_SHARED|MAP_ANONYMOUS)` created in MINIT, so forked workers share it at the same address (pointers are valid everywhere); threads (ZTS) share it anyway.

## Layout

`[ header | workers | segment table | slot tables ][ segment 0 ][ segment 1 ] ...`
- Segment size = power of two so that there are at most 256 segments (min 64 KB). Entries larger than half a segment take a run of contiguous segments.
- Entry = `stash_entry` header (hash, size, expiry, seq, 32-bit created, key length/offset, live/hot/flags, the user's 32-bit `user`: 48 bytes, `_Static_assert`ed) + the user's block. The PHP block is `zval` + key `zend_string` + value, written as opcache-style immutable structures
  (refcount 2, `IS_ARRAY_IMMUTABLE` / interned strings, hash precomputed), so a reader can hand the pointer straight to userland.
  Equal array keys are stored once per entry (rows of a result set share their column names): a 256-slot table by hash (`STASH_G(keys)`, `key_slot`/`key_hit`) remembers this value's keys; a miss overwrites the slot.
  Its decisions depend only on the sequence of key contents, so sizing (`size_zval`) and persisting (`persist_*`) dedupe exactly alike and agree on the size.
- Slot: 64 bits = `{ offset/8 (32 bit), stash_slot_tag(hash) }`; 0 empty. The tag is the hash times an odd constant, top 32 bits; its low bits are the home slot, all 32 the probe filter: a probe compares them first and only dereferences on a match.
  Not the raw hash: PHP's DJBX33A gives keys like `prefix.N` regular low bits, and as home slots they formed probe runs of hundreds at 3/4 fill (simulated: hit 215 / miss 1247 probes vs 2 / 8).

## Shards: fixed slices

The key hash (Fibonacci mix of the high bits) picks one of up to 8 shards. Shards exist only while each one keeps >= 8 MB and >= 16 segments: 1 shard up to 8 MB, 2 at 16 MB, 4 at 32 MB (default), 8 from 64 MB.
A shard owns, for the arena's life: its lock (alone on its cache line), a fixed slot table, a fixed contiguous slice of segments `[lo, hi)`, one open segment per TTL bucket, its expiry cursor, live bytes and `nlive`.
Every write takes only its key's shard lock and only ever touches that shard's slots and segments: there is no shared pool and no cross-shard work, so a walk over a segment cannot race another shard reusing it.
Price: an entry must fit in one slice (larger ones are refused), and a shard full of data cannot borrow memory from an idle one (keys hash evenly, so this matters only for a few huge keys).

Locks: test-and-test-and-set, inline fast path (`stash_lock`), slow path `stash_lock_wait` yields after 128 spins. The lock word is the holder's owner id = pid | process start time << 32.
A waiter that has yielded ~1000 times checks `owner_dead(holder)` (gone, a zombie, or the pid now belongs to a newer process: start time differs; Linux `/proc/<pid>/stat`, macOS `sysctl(KERN_PROC_PID)`)
and takes the lock over with a CAS (`lock_steals` in `stash_stats()`), then runs `shard_repair()`: the slot table is the truth (it only changes by single stores), everything else is rebuilt from it.
An odd `mseq` (a delete died mid-shift: every miss in the shard would retry forever) is made even and the key it left in two slots loses its second one; each segment's `live` comes from the entries slots reach (an entry unlinked halfway stops counting),
`pending` from the worker slots' `wip` records (an abandoned allocation stops holding its segment), states from the runs (a run taken or freed halfway, an open segment never installed). O(slots + entries) under the lock, only after a steal.
A writer that dies filling a big entry outside the lock holds no lock: before unlocking it records the entry in its worker slot (`stash_wip`, `wip` changes only under that entry's shard lock), and the entry is aborted when the slot is claimed again,
by `apcu_clear_cache`, or when an allocation fails (`wip_reap`, at most once a second per shard). Tests: `tests/c/repair.c` (each half-done state, deterministic), 037 (40 kill -9s of a writer mid inline/big put/delete).
Robust pthread mutexes would let the kernel report a dead holder, but measured on Linux they are 15% slower uncontended and 2-8x slower with 2-16 contending processes (futex sleeps, also with a trylock spin in front). Not used.

## Slot tables: fixed, backward-shift deletes

Sized once: ~2 slots per `apc.entries_hint` and at least one per 256 bytes of arena (3% of it), split evenly over the shards. A shard holds at most 3/4 of its slots (`cap`);
an insert into a full table evicts (merge, below) like a full memory does. So a cache of very small entries evicts before its memory is full: raise `apc.entries_hint` for that.
Deletes use backward shift (linear probing without tombstones): later entries of the probe run move back into the hole, so probe runs never fill up with tombstones.
A reader that probes past a key while a delete moves it back would miss it: deletes that move anything bump the shard's `mseq` (odd while moving, its own cache line),
and `stash_find` reads `mseq` before probing and treats a miss as real only if it is unchanged and even (seqlock); hits need no check (pin + slot re-check below).

## Writes

A write is a `stash_put`: `stash_put_begin` locks the key's shard, `stash_alloc`s (bump-allocate in the shard's open segment for the entry's TTL bucket, or take a fresh one / a run) and sets the header;
the caller (`build()` in stash.c: size the value, `persist_entry`) fills key and payload; `stash_put_commit` locates the slot, publishes with a release store, marks the old entry dead and unlocks.
Blocks up to `STASH_INLINE_PUT` (2 KB) are filled under the lock (one critical section); larger ones are filled with the lock dropped and recorded as the worker's `wip` meanwhile (see Locks). The caller never sees the lock.
Per segment: `used` (bytes appended), `live`, `pending` (allocated, unpublished). A sealed segment with `live==0 && pending==0` becomes DEAD (`seg_check`); `reclaim()` frees DEAD segments no worker pins, under the shard lock.
TTL buckets (Segcache): one open segment per bucket (ttl 0, < 8 s, < 64 s, < 512 s, < 4096 s, < 32768 s, longer); each segment keeps `max_exp`. Every write runs `stash_expire_step()` on its shard: a cursor walks the entries of
the shard's segments with `max_exp <= now`, at most 64 per write, and unlinks them; after a full pass with nothing to do it idles until the next second. `exp_gen` vs the segment's `gen` (bumped on every free) keeps a cursor from resuming mid-entry in a reused segment.
When an allocation fails: `reclaim()` -> `stash_expire_step()` with a large budget -> `stash_merge()` repeatedly.

## Merge (Segcache-style): compaction and eviction in one

`stash_merge()` picks up to 4 victims among the shard's sealed segments (`pick_sparse`, else `pick_oldest`; then `move_survivors`, `evict_rest`):
- if there are at least two sparse ones (<= 1/4 live, together <= one segment): compaction, every live entry moves forward (`relocate`: into its segment's TTL bucket, slot repointed), nothing is lost;
- else the 4 oldest (`born`, the inserts counter when the segment was taken): of the entries read since they were written or last moved, the most-read move forward, up to one segment's worth, the rest is evicted (`expunges` counts the round, before the unlinks; the shard's `evicted` / `expired` count the entries).
  Ranking: bytes per `hot` level are summed over the victims; levels that fit the budget all move, the next level down fills what is left first come. A moved copy keeps half its count (aging), so it must be read again to stay on top. A big entry's run is evicted when oldest, unless it was read: then it goes to the back once (count halved).
Ghost (S3-FIFO): per shard, one 32-bit tag per slot (in the header after the slot tables), direct-mapped by a Fibonacci mix of the hash below the shard's bits. An eviction writes the key's tag;
a new key whose tag is there starts at `hot = 2` (survives two merges unread; `ghost_hits`). Keys used once never come back, so only recurring keys whose reuse distance exceeds the cache get the extension.
`hot` is a byte in the entry header, a Morris-style read counter (0-15): the first read sets 1, later reads add 1 with probability 1/2^hot (per-worker xorshift), so a hot key writes its cache line ~log2(reads) times between merges and it stays shared otherwise (exact per-entry hit counters would write on every read).
Victims that readers pin die but stay allocated until unpinned; the next round moves on.
Moving an entry is a `memcpy` of its block plus the user's `stash_rebase(e, from)`: stash.c walks the value and shifts every pointer that pointed into the old block (arrays' `arData`, bucket keys, strings, nested arrays) by the distance moved; `zend_empty_array` stays.
Value strings get `h = 0` (a reader may have been hashing the old copy during the memcpy). Nothing is re-persisted, so the moved block has the old size by construction.

## Reads (lock-free) and pins

`stash_find()`: load the slot, compare tag, then `stash_hold(entry)` = publish the entry's segment bit in this worker's bitmap (seq_cst store + fence), re-check the slot is unchanged, only then dereference.
Slot tables never move, so readers use a process-local copy of their addresses (`stash_tabs`) and touch no header cache line that writers write.
Bits set during a call outlive it as idle pins (re-pinning costs a full fence per call). They are dropped at a call end once a writer bumps `unpin_gen` (`reclaim()` found a dead segment still pinned) or past `nsegs/16` idle pins, by a writer before it allocates (its own pins would block its reclaim), before a FrankenPHP worker waits for a request, and at request end. Ceiling: a process idle between calls holds up to `nsegs/16` segments. Not dropped early are segments a zero-copy pointer escaped from (`stash_keep`): those stay until post-deactivate (not RSHUTDOWN: session and other built-ins run their RSHUTDOWN after ours and still serialize user values). There is no earlier release: userland can keep any value until the request ends.
`reclaim()` first collects the shard's DEAD set, then a seq_cst fence, then ORs the bitmaps of all workers up to the high-water mark `nworkers`, and frees DEAD and unpinned segments.
That order matters: reading pins first would let a segment that died afterwards be freed under a reader that pinned it just before.
A worker whose bits block a free has its owner checked (at most once a second per slot); a dead one has its bits cleared. Forked children get a new owner id and worker slot (`pthread_atfork`) and inherit the parent's bits.

## Threads (ZTS) and FrankenPHP

Per-thread state (`stash_ctx`: worker slot and pin bitmaps; the key table; held leases) lives in module globals: plain globals under NTS, TSRM under ZTS. Each thread claims a worker slot in RINIT and gives it back in GSHUTDOWN (thread end).
Threads share the owner id of their process: a lock held by a thread is only taken over once its whole process is gone.
FrankenPHP: classic requests are short, so `auto` gives them zero-copy. A worker script serves many requests inside one PHP request through `frankenphp_handle_request()`, whose handler stash wraps:
the worker switches to copy mode at its first handled request, also with `stash.request_scope=1` (it may keep fetched values across requests: Symfony's container does). Values fetched while booting stay pinned until the worker ends.

## Fetch modes

- zero-copy (`zero_copy`, set in RINIT from `request_scope`): returns the arena pointer; the segment stays pinned until the request ends.
- copy: `copy_out` = `zend_array_dup` (one memcpy of an immutable array) + `unshare_contents` (repoint arena strings/keys). The same key table maps a deduped arena key to its first private copy, which later rows share (refcounted).
- serialized entries (objects, shared refs): `cache_unserialize` with a private unserialize state.

## Single-flight and early refresh (`stash_remember`, `apcu_entry`)

A lease is not a cache entry: each shard has 32 fixed `stash_lease` slots `{key hash, owner id, worker slot, until}` in the header, read and written under that shard's lock (the key's own shard).
`stash_lease_take` returns TAKEN, HELD, MINE (recursion: our own callback asks for its key) or FULL (32 concurrent computes in the shard: compute without one). An expired lease (30 s) is free;
a holder found dead (`owner_dead`, checked outside the lock since it reads /proc) is replaced on a second pass only if it still holds, and `stash_lease_drop` clears a lease only if this worker slot still holds it, so nobody drops a lease another worker retook.
Leases are keyed by the 64-bit hash: two keys with the same hash share one (one waits for the other, then computes). Waiters poll (50 us doubling to 5 ms) for the value. Held leases are listed in module globals and dropped in post-deactivate if a callback bailed out.
`stash_stats()['leases']` counts the held ones; `APCUIterator`, `apcu_cache_info` and `apcu_clear_cache` never see them.
The callback's run time (ms) is kept in the header's `user` field (the arena copies it when merge moves the entry, test 036). XFetch on a hit: refresh when `now - delta * beta * ln(rand) >= expires`; the refresher needs the lease, the rest return the current value.

## APCu layer

`stash.apcu_compat` (default 1, forced to 0 when APCu is loaded) decides in MINIT: `stash_apcu_minit()` registers the `apcu_*` functions (own stub, `stash_apcu_arginfo.h`), `APCUIterator`, the constants, `apc.enabled`/`apc.enable_cli` and the aliases `apc.shm_size`/`apc.entries_hint`/`apc.ttl`; with 0 none of that.
The sizing INI is always `stash.*`. An alias applies its value to the `stash.*` entry only when it is configured and the `stash.*` name is not (`OnAlias`), then shows the value in effect (`sync_aliases`). Both go through `ini_put()`, never `zend_alter_ini_entry`: that one records the entry in `EG(modified_ini_directives)`, emalloc'ed memory that `php_module_startup` frees at its end, and the next `ini_set()` of a new directive wrote into it (segfault in Symfony's boot).

`apcu_*` functions are thin wrappers over the store API in `stash_php.h`; `APCUIterator` and `apcu_cache_info` use `stash_scan` (lock-free over the slot tables, `stash_scan_next`: may skip or repeat entries a concurrent write moves).
`apcu_sma_info` reports one segment per shard (its slice and that slice's free runs): an entry only fits into its own shard's free space.
`apcu_inc/dec/cas` go through `stash_add_long`/`stash_cas_long`: the integer is modified in place under the shard lock with relaxed atomics (readers copy that word). `apcu_clear_cache` takes all shard locks in index order.

## Invariants worth remembering

- Never allocate (emalloc, zvals) while holding a shard lock: an OOM bailout (PHP <= 8.6) or the hard-limit `_exit` (8.7+) would leave it locked for every worker. `stash_rebase` runs under the lock: it only rewrites pointers.
- Never take a second shard lock while holding one, except `apcu_clear_cache` (all, in index order).
- Arena strings are `IS_STR_INTERNED` but never `IS_STR_PERMANENT`: that flag promises process lifetime and PCRE then keeps the pointer as a cache key.
- Only keys carry a hash. Value strings are stored with `h == 0`; PHP hashes them on first use as a key and writes `h` into the arena, a benign idempotent race.
- Never dereference an entry before `stash_hold` + re-check (`-DSTASH_POISON`, set by tests/c, fills freed segments with 0x5A to catch this).
- A live entry is always reachable from exactly one slot; `relocate()` relies on it.
- Key dedupe must stay a function of key contents only: sizing and persisting must agree on `e->size` (`ZEND_ASSERT` at the end of `persist_entry`).
- Every pointer inside a block points into the same block or outside the arena (`zend_empty_array`): `stash_rebase` relies on it.
- Only the owner shard, under its lock, changes a segment's state; DEAD ends only in its `reclaim()`. So a walk over a segment (`expire_step`, `merge`) may run past the point where its last unlink made it DEAD: the memory stays until this lock is dropped.
- `seg_check()` is what moves SEALED -> DEAD; call it after any change of `live`/`pending`.
- `seg_carve()` sets an entry's size before `used` covers it, so `shard_repair` can always walk a segment. Entry sizes are multiples of 8 (slots hold offset/8).
- The slot table never holds more than `cap` (3/4) entries: probes and backward shifts rely on an empty slot ending every run.
- Tests that must see pressure use a small `apc.shm_size` (2M-4M, one shard). 015 (16M, 2 shards, 8 processes) and 018 (deletes shifting keys under readers) are the concurrency tests:
  run `docker/stress.sh` after touching locking, reclamation or the slot table; a single pass catches races only sometimes. 018 fails reliably if `stash_find` stops retrying misses.
