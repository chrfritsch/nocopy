# Architecture

The internals: data layout, locking, eviction, the read path, the Valkey tier and the invariants every change must keep.
Start with [How nocopy works](how-it-works.md) for the overview; this page is for people changing the C code.

## Layers

Layers, each using only the one below it:
- Arena (no PHP): entries are a header plus the caller's bytes. `nocopy_arena.h` is its public API; the read path (`nocopy_find`, pins, the lock fast path) is inline there. `nocopy_arena_int.h` is shared by its units only:
  `nocopy_arena.c` (setup, locks and `shard_repair`, worker slots and pins, segments, slot tables, writes, stats), `nocopy_evict.c` (expiry, merge, ghost), `nocopy_lease.c` (single-flight leases), `nocopy_owner.c` (owner ids: pid + start time).
- `nocopy_value.c`: PHP values in and out of an arena block (size, persist, rebase, copy-out, serialize, `nocopy_build`).
- `nocopy_store.c`: the store API in `nocopy_php.h` (`nocopy_store`/`fetch`/`drop`/`add_long`/`cas_long`/`remember`), copy mode's private copies, leases.
- `nocopy.c`: `nocopy_*`, INI, the module, FrankenPHP's worker hook.
- `nocopy_valkey.c` (optional, `--disable-nocopy-valkey` leaves it out): the Valkey tier. `nocopy_store.c` reaches it only through `nocopy_vk_*` in `nocopy_php.h` (inline stubs without it), and only on `detour`; the tier uses the local store's pieces exported there (`nocopy_build`/`publish`/`fetch_local`/`serialize`/`unserialize`).
- `apcu/apcu.php` (PHP, not built): `apcu_*` over `nocopy_*` for code that cannot switch. The extension only registers `apc.enabled`/`apc.enable_cli` (when APCu is not loaded), which callers check before using `apcu_*`.
- No arena (`nocopy.shm_name` not set or not attached, `nocopy_available()` false): every store-API function returns its miss or failure (stores still size and validate the value, so the caller's errors throw), `nocopy_remember` computes without keeping, `nocopy_tracked()` does not throw, RINIT claims no worker slot. `nocopy_shared_with()`: `none`.

`tests/c/stress.c` drives the arena directly from threads (plain and ThreadSanitizer, `./dev c`); `tests/c/repair.c` uses the internal header. Both supply `nocopy_rebase()`, the one callback the arena needs from its user (see Merge).

Everything lives in one POSIX shm object named by `nocopy.shm_name` (required), mapped in MINIT at the same address in every process that attaches, forks included (pointers are valid everywhere); threads (ZTS) share it anyway (`nocopy_arena_open`, `nocopy_arena.c`):
- Name `base.<8 hex>`: the layout id, a hash of the loaded `nocopy.so` (plus nocopy version and `ZEND_EXTENSION_BUILD_ID`), so processes share only when they run the same code. Mode 0600.
- Liveness: every process that maps it holds a shared `flock` on its object for life (fd kept open, inherited by forks, closed on exec). `LOCK_EX|LOCK_NB` succeeds only on an object no process maps.
- Create: `O_EXCL`, then `LOCK_SH` and a check that the name still refers to this object (`hold`: same `st_dev`/`st_ino`; someone may have removed it as unused in between),
  then `posix_fallocate` (Linux: a sparse tmpfs file past `/dev/shm`'s limit SIGBUSes on first touch), map at the first free address of `bases[]` (0x2000'0000'0000, else 0x10'0000'0000; `MAP_FIXED_NOREPLACE`), set up the header, store `magic` last (release).
- Attach: map the header anywhere, wait up to 2 s for `magic`; while it is missing and the object is unused, its creator died: `remove_unused` unlinks it (under `LOCK_EX`, only if the name still refers to it) and we start over. Then check `abi`, `hold`, map at `base`.
  Test 060: 24 processes starting at once over an unfinished object all end in one arena; without the re-check in `hold`, most end up in orphans.
- `collect` (Linux, `/dev/shm`): other builds' objects under the same base are removed when unused; the first one in use is reported (MINIT warns: those processes do not share this cache).
- No name, or any failure, leaves `nocopy_a` NULL: the process has no cache (below), never a private one, whose writes the others would not see.

## Layout

`[ header | workers | segment table | slot tables ][ segment 0 ][ segment 1 ] ...`
- Segment size = power of two so that there are at most 256 segments (min 64 KB). Entries larger than half a segment take a run of contiguous segments.
- Entry = `nocopy_entry` header (hash, size, expiry, seq, 32-bit created, key length/offset, live/hot/flags, the user's 32-bit `user`: 48 bytes, `_Static_assert`ed) + the user's block. The PHP block is `zval` + key `zend_string` + value, written as opcache-style immutable structures
  (refcount 2, `IS_ARRAY_IMMUTABLE` / interned strings, hash precomputed), so a reader can hand the pointer straight to userland.
  Equal array keys are stored once per entry (rows of a result set share their column names): a 256-slot table by hash (`NOCOPY_G(keys)`, `key_slot`/`key_hit`) remembers this value's keys; a miss overwrites the slot.
  Its decisions depend only on the sequence of key contents, so sizing (`size_zval`) and persisting (`persist_*`) dedupe exactly alike and agree on the size.
- Slot: 64 bits = `{ offset/8 (32 bit), nocopy_slot_tag(hash) }`; 0 empty. The tag is the hash times an odd constant, top 32 bits; its low bits are the home slot, all 32 the probe filter: a probe compares them first and only dereferences on a match.
  Not the raw hash: PHP's DJBX33A gives keys like `prefix.N` regular low bits, and as home slots they formed probe runs of hundreds at 3/4 fill (simulated: hit 215 / miss 1247 probes vs 2 / 8).

## Shards and the segment pool

The key hash (Fibonacci mix of the high bits) picks one of up to 8 shards. Shards exist only while each one keeps >= 8 MB and >= 16 segments: 1 shard up to 8 MB, 2 at 16 MB, 4 at 32 MB (default), 8 from 64 MB.
A shard owns, for the arena's life: its lock (alone on its cache line), a fixed slot table, one open segment per TTL bucket, its expiry cursor, live bytes and `nlive`; and, while it holds them, the segments it took from the pool.
Every write takes only its key's shard lock and only touches that shard's slots and segments.
- Pool: `owner[s]` (header, one byte per segment) is the shard index + 1, 0 while the segment is free. A shard claims with a CAS 0 -> its id (under its own lock) and releases with a store of 0 after zeroing the segment (`seg_free`, from `reclaim()`), so a segment changes shard only through the pool, zeroed, and its old shard never walks it again.
  The shard's `own` bitmap and `nown` index its segments; `owner[]` is the truth (`shard_repair` rebuilds both from it and returns segments claimed but never set up, or freed halfway).
- `take_run` scans `owner[]` from the shard's home range (`idx * nsegs / nshards`): balanced shards stay contiguous, as with fixed slices. A plain take leaves one free segment per shard, for merges (`reserve`).
- Balance: a shard below 15/16 of its share (`nsegs / nshards`) that finds the pool dry has the shard holding the most evict once (`rebalance`: drop own lock, lock that one, `reclaim` + `nocopy_merge`, unlock, relock) before it merges its own. Above its share it merges its own, as before.
  So a hot shard borrows what others do not use and gives it back when they need it; idle memory is never taken from a shard that is not over its share.
Price: an entry must still fit in a shard's share (larger ones are refused; fragmentation would make bigger runs fail anyway).

Locks: test-and-test-and-set, inline fast path (`nocopy_lock`), slow path `nocopy_lock_wait` yields after 128 spins. The lock word is the holder's owner id = pid | process start time << 32.
A waiter that has yielded ~1000 times checks `owner_dead(holder)` (gone, a zombie, or the pid now belongs to a newer process: start time differs; Linux `/proc/<pid>/stat`, macOS `sysctl(KERN_PROC_PID)`)
and takes the lock over with a CAS (`lock_steals` in `nocopy_stats()`), then runs `shard_repair()`: the slot table is the truth (it only changes by single stores), everything else is rebuilt from it.
An odd `mseq` (a delete died mid-shift: every miss in the shard would retry forever) is made even and the key it left in two slots loses its second one; each segment's `live` comes from the entries slots reach (an entry unlinked halfway stops counting),
`pending` from the worker slots' `wip` records (an abandoned allocation stops holding its segment), states from the runs (a run taken or freed halfway, an open segment never installed). O(slots + entries) under the lock, only after a steal.
A writer that dies filling a big entry outside the lock holds no lock: before unlocking it records the entry in its worker slot (`nocopy_wip`, `wip` changes only under that entry's shard lock), and the entry is aborted when the slot is claimed again,
by `nocopy_clear`, or when an allocation fails (`wip_reap`, at most once a second per shard). Tests: `tests/c/repair.c` (each half-done state, deterministic), 037 (40 kill -9s of a writer mid inline/big put/delete).
Robust pthread mutexes would let the kernel report a dead holder, but measured on Linux they are 15% slower uncontended and 2-8x slower with 2-16 contending processes (futex sleeps, also with a trylock spin in front). Not used.

## Slot tables: fixed, backward-shift deletes

Sized once: ~2 slots per `nocopy.entries_hint` and at least one per 256 bytes of arena (3% of it), split evenly over the shards. A shard holds at most 3/4 of its slots (`cap`);
an insert into a full table evicts (merge, below) like a full memory does. So a cache of very small entries evicts before its memory is full: raise `nocopy.entries_hint` for that.
Deletes use backward shift (linear probing without tombstones): later entries of the probe run move back into the hole, so probe runs never fill up with tombstones.
A reader that probes past a key while a delete moves it back would miss it: deletes that move anything bump the shard's `mseq` (odd while moving, its own cache line),
and `nocopy_find` reads `mseq` before probing and treats a miss as real only if it is unchanged and even (seqlock); hits need no check (pin + slot re-check below).

## Writes

A write is a `nocopy_put`: `nocopy_put_begin` locks the key's shard, `nocopy_alloc`s (bump-allocate in the shard's open segment for the entry's TTL bucket, or take a fresh one / a run) and sets the header;
the caller (`nocopy_build()` in nocopy_value.c: size the value, `persist_entry`) fills key and payload; `nocopy_put_commit` locates the slot, publishes with a release store, marks the old entry dead and unlocks.
Blocks up to `NOCOPY_INLINE_PUT` (2 KB) are filled under the lock (one critical section); larger ones are filled with the lock dropped and recorded as the worker's `wip` meanwhile (see Locks). The caller never sees the lock.
Per segment: `used` (bytes appended), `live`, `pending` (allocated, unpublished). A sealed segment with `live==0 && pending==0` becomes DEAD (`seg_check`); `reclaim()` frees DEAD segments no worker pins, under the shard lock.
TTL buckets (Segcache): one open segment per bucket (ttl 0, < 8 s, < 64 s, < 512 s, < 4096 s, < 32768 s, longer); each segment keeps `max_exp`. Every write runs `nocopy_expire_step()` on its shard: a cursor walks the entries of
the shard's segments with `max_exp <= now`, at most 64 per write, and unlinks them; after a full pass with nothing to do it idles until the next second. `exp_gen` vs the segment's `gen` (bumped on every free) keeps a cursor from resuming mid-entry in a reused segment.
When an allocation fails: `reclaim()` -> `nocopy_expire_step()` with a large budget -> `nocopy_merge()` repeatedly.

## Merge (Segcache-style): compaction and eviction in one

`nocopy_merge()` picks up to 4 victims among the shard's sealed segments (`pick_sparse`, else `pick_oldest`; then `move_survivors`, `evict_rest`):
- compaction if the sparsest sealed ones fit together into one segment (at least two), or the sparsest alone is at most 3/4 live: every live entry moves forward (`relocate`: into its segment's TTL bucket, slot repointed), nothing is lost;
- else 4 victims among the 32 oldest (`born`, the inserts counter when the segment was taken): the oldest and the 3 with the fewest `reads` (per segment: the sum of its live entries' `2^hot - 1`, adjusted at birth, death and by readers on each `hot` bump; racy, signed, reset when taken); of the entries read since they were written or last moved, the most-read move forward, up to one segment's worth, the rest is evicted (`expunges` counts the round, before the unlinks; the shard's `evicted` / `expired` count the entries).
  Ranking: bytes per `hot` level are summed over the victims; levels that fit the budget all move, the next level down fills what is left first come. A moved copy keeps half its count (aging), so it must be read again to stay on top. A big entry's run is evicted when oldest, unless it was read: then it goes to the back once (count halved).
Ghost (S3-FIFO): per shard, one 32-bit tag per slot (in the header after the slot tables), direct-mapped by a Fibonacci mix of the hash below the shard's bits. An eviction writes the key's tag;
a new key whose tag is there starts at `hot = 2` (survives two merges unread; `ghost_hits`). Keys used once never come back, so only recurring keys whose reuse distance exceeds the cache get the extension.
`hot` is a byte in the entry header, a Morris-style read counter (0-15): the first read sets 1, later reads add 1 with probability 1/2^hot (per-worker xorshift), so a hot key writes its cache line ~log2(reads) times between merges and it stays shared otherwise (exact per-entry hit counters would write on every read).
Victims that readers pin die but stay allocated until unpinned; the next round moves on.
Moving an entry is a `memcpy` of its block plus the user's `nocopy_rebase(e, from)`: nocopy_value.c walks the value and shifts every pointer that pointed into the old block (arrays' `arData`, bucket keys, strings, nested arrays) by the distance moved; `ARENA_EMPTY` stays.
Value strings get `h = 0` (a reader may have been hashing the old copy during the memcpy). Nothing is re-persisted, so the moved block has the old size by construction.

## Reads (lock-free) and pins

`nocopy_find()`: load the slot, compare tag, then `nocopy_hold(entry)` = publish the entry's segment bit in this worker's bitmap (seq_cst store + fence), re-check the slot is unchanged, only then dereference.
Slot tables never move, so readers use a process-local copy of their addresses (`nocopy_tabs`) and touch no header cache line that writers write.
Bits set during a call outlive it as idle pins (re-pinning costs a full fence per call). They are dropped at a call end once a writer bumps `unpin_gen` (`reclaim()` found a dead segment still pinned) or past `nsegs/16` idle pins, by a writer before it allocates (its own pins would block its reclaim), before a FrankenPHP worker waits for a request, and at request end. Ceiling: a process idle between calls holds up to `nsegs/16` segments. Not dropped early are segments a zero-copy pointer escaped from (`nocopy_keep`): those stay until post-deactivate (not RSHUTDOWN: session and other built-ins run their RSHUTDOWN after ours and still serialize user values). There is no earlier release: userland can keep any value until the request ends.
`reclaim()` first collects the shard's DEAD set, then a seq_cst fence, then ORs the bitmaps of all workers up to the high-water mark `nworkers`, and frees DEAD and unpinned segments.
That order matters: reading pins first would let a segment that died afterwards be freed under a reader that pinned it just before.
A worker whose bits block a free has its owner checked (at most once a second per slot); a dead one has its bits cleared. Forked children get a new owner id and worker slot (`pthread_atfork`) and inherit the parent's bits.

## Threads (ZTS) and FrankenPHP

Per-thread state (`nocopy_ctx`: worker slot and pin bitmaps; the key table; held leases) lives in module globals: plain globals under NTS, TSRM under ZTS. Each thread claims a worker slot in RINIT and gives it back in GSHUTDOWN (thread end).
Threads share the owner id of their process: a lock held by a thread is only taken over once its whole process is gone.
FrankenPHP: classic requests are short, so `auto` gives them zero-copy. A worker script serves many requests inside one PHP request through `frankenphp_handle_request()`, whose handler nocopy wraps:
the worker switches to copy mode at its first handled request (it may keep fetched values across requests: Symfony's container does). Values fetched while booting stay pinned until the worker ends.

## Fetch modes

- zero-copy (`zero_copy`, set in RINIT: only under short-lived SAPIs, `short_lived_sapi()`): returns the arena pointer; the segment stays pinned until the request ends.
- copy: `copy_out` = `zend_array_dup` (one memcpy of an immutable array) + `unshare_contents` (repoint arena strings/keys). The same key table maps a deduped arena key to its first private copy, which later rows share (refcounted).
  Kept copies (`nocopy_local`, `nocopy.local_size`): a 4-way set-associative table (1024 sets of 4) in request memory, by entry address, holds `{entry, seq, size, copy}`. A fetch whose entry has the same address and `seq` returns the kept copy (refcount + 1; COW protects it); otherwise it copies, and keeps the copy only if the slot's `seen` is this entry (second sighting, TinyLFU's doorkeeper), else just records it in `seen`. A miss takes the way with neither a copy nor a pending `seen`, else one without a copy, else one the SIEVE hand found unused (taking any empty way first let new keys overwrite each other's `seen`, and a loop kept nothing). Keeping every copy made fetches of 100k distinct strings 37 -> 64 ns (freeing cold evictees).
  `(address, seq)` is one version: segments belong to one shard for life and `seq` only grows per shard, so a reused address has a larger `seq`; a merge move changes the address (one re-copy). Integers changed in place (`nocopy_inc`) are never kept. Over budget a SIEVE hand (`local_hand`) clears `visited` bits (set on each kept-copy hit) and drops unvisited copies until the new one fits; freed in RSHUTDOWN (later fetches copy without keeping).
- serialized entries (objects, shared refs): `cache_unserialize` with a private unserialize state.

## Single-flight and early refresh (`nocopy_remember`)

A lease is not a cache entry: each shard has 32 fixed `nocopy_lease` slots `{key hash, owner id, worker slot, until}` in the header, read and written under that shard's lock (the key's own shard).
`nocopy_lease_take` returns TAKEN, HELD, MINE (recursion: our own callback asks for its key) or FULL (32 concurrent computes in the shard: compute without one). An expired lease (30 s) is free;
a holder found dead (`owner_dead`, checked outside the lock since it reads /proc) is replaced on a second pass only if it still holds, and `nocopy_lease_drop` clears a lease only if this worker slot still holds it, so nobody drops a lease another worker retook.
Leases are keyed by the 64-bit hash: two keys with the same hash share one (one waits for the other, then computes). Waiters poll (50 us doubling to 5 ms) for the value; meanwhile a local (not Valkey) waiter with `beta > 0` takes an expired entry (`nocopy_find_ex(stale)`, `nocopy_stale_ok`: expired less than its TTL ago). Held leases are listed in module globals and dropped in post-deactivate if a callback bailed out.
`nocopy_stats()['leases']` counts the held ones; `entries` and `nocopy_clear` never see them.
The callback's run time (ms) is kept in the header's `user` field (the arena copies it when merge moves the entry, test 036). XFetch on a hit: refresh when `now - delta * beta * ln(rand) >= expires`; the refresher needs the lease, the rest return the current value.

## Counters and INI

`nocopy_inc`/`nocopy_cas` go through `nocopy_add_long`/`nocopy_cas_long`: the integer is modified in place under the shard lock with relaxed atomics (readers copy that word). `nocopy_clear` takes all shard locks in index order.
INI values set in MINIT (`nocopy.serializer` in effect) go through `ini_put()`, never `zend_alter_ini_entry`: that one records the entry in `EG(modified_ini_directives)`, emalloc'ed memory that `php_module_startup` frees at its end, and the next `ini_set()` of a new directive wrote into it (segfault in Symfony's boot).

## Valkey tier (`nocopy_valkey.c`)

`nocopy.valkey` makes Valkey the cache of record; the arena holds local copies. `nocopy_store.c` hands `nocopy_store`/`fetch`/`drop`/`add_long`/`cas_long`, the multi variants,
`compute` and `nocopy_clear_all` to `nocopy_vk_*` (with an arena; without one they miss or fail in `nocopy_store.c`); `nocopy_valkey.c` owns the routing, the connection and every command.
- Connections are libvalkey's (vendored in `third_party/libvalkey`, `scripts/vendor-libvalkey.sh`; compiled with `-w`, `-U_GNU_SOURCE` and its API hidden): blocking `valkeyContext`s with `nocopy.valkey_timeout` for connect and every read and write,
  `VALKEY_OPT_NO_PUSH_AUTOFREE` so pushes come back as replies and are applied here. Commands are built as RESP by hand (the key prefix without a copy) and queued with `valkeyAppendFormattedCommand`.
- TLS (`tls://`, `rediss://`, `HAVE_NOCOPY_TLS`): our `SSL_CTX` from MINIT, shared by forks and threads; our `SSL` (SNI, host or IP verified) handed to `valkeyInitiateTLS`. libvalkey frees it without close_notify, so a forked child leaves the parent's session intact.
  Its socket BIO writes without `MSG_NOSIGNAL`: PHP's SAPIs (CLI, php-fpm, FrankenPHP) ignore SIGPIPE, the listener does so itself, macOS sockets get `SO_NOSIGPIPE`. The listener polls with 0 ms while `SSL_pending`. Errors leave nothing in OpenSSL's per-thread queue (PHP's openssl functions read it).
- A command connection per worker (`NOCOPY_G(vk)`), lazy. Handshake: `HELLO 3 [AUTH] SETNAME nocopy`, `SUBSCRIBE` to an idle channel (exempt from Valkey's `timeout`).
  No tracking: a reconnect needs no clear. Any error or timeout closes it (the reply stream would be out of step); a new attempt waits 0.5-1.5 s.
- One listener per arena: a process of its own (double fork, `setsid`, signals reset, every inherited fd closed, stdio on /dev/null, `nocopy-valkey`), running only C:
  one connection with `CLIENT TRACKING ON BCAST PREFIX p NOLOOP`, `poll` at 100 ms, applies pushes, `PING` every second (no answer within max(1 s, timeout): reconnect).
  Header: `vk_beat` (monotonic ms, every poll round), `vk_pid`, `vk_live` (connected). Every (re)connect bumps `inval_gen` and clears the arena: what it missed is unknown.
- `nocopy_vk_ready()` once per request (`synced`, reset in RINIT and per FrankenPHP request): command connection up, and the listener live (`vk_live` and a beat under 1 s old).
  No beat for 1 s: the first worker to swap `vk_beat` (CAS) spawns a new listener; workers wait up to the timeout for `vk_live`. A listener whose `vk_pid` is not its own exits. While not ready nothing local is served.
- Lifetime: the listener drops its shared flock (so the segment can still be collected) and exits when `/dev/shm/<name>` no longer is its object (`nocopy_arena_current`, `stat`: fpm workers run as another user than the segment's owner).
- Push `invalidate [keys]`: `inval_gen++` (seq_cst) first, then `nocopy_drop_local` per key; a null list (FLUSHALL) clears the arena.
- Fill guard: `vk_fetch` reads `inval_gen` before its `GET` and drops the copy it just stored if the counter moved. The listener bumps before it drops, so either the filler sees the bump or the drop comes after the store.
- Writes: local first, then `SET` (an invalidation from elsewhere can only remove our copy, never be overtaken by it). `add`: `SET NX` first, local after. The listener gets our own write pushed too and drops the fresh copy: one refetch per write per server.
- Encoding: integers as decimal (for `INCRBY`); else `0x00, flags, int64 expires, uint32 compute ms` + the serialized value. `PXAT` = (expires + 1) s.

## Invariants worth remembering

- Never allocate (emalloc, zvals) while holding a shard lock: an OOM bailout (PHP <= 8.6) or the hard-limit `_exit` (8.7+) would leave it locked for every worker. `nocopy_rebase` runs under the lock: it only rewrites pointers.
- Never take a second shard lock while holding one, except `nocopy_clear` (all, in index order). `rebalance` drops its own first: `nocopy_alloc` may unlock and relock, so nothing before it in a write may rely on shard state.
- A segment changes shard only through the pool: zeroed, `owner[s] = 0` (release), claimed by CAS. Walk a shard's segments by `own` (`SH_SEGS`), never by index range.
- Arena strings are `IS_STR_INTERNED` but never `IS_STR_PERMANENT`: that flag promises process lifetime and PCRE then keeps the pointer as a cache key.
- Only keys carry a hash. Value strings are stored with `h == 0`; PHP hashes them on first use as a key and writes `h` into the arena, a benign idempotent race.
- Never dereference an entry before `nocopy_hold` + re-check (`-DNOCOPY_POISON`, set by tests/c, fills freed segments with 0x5A to catch this).
- A live entry is always reachable from exactly one slot; `relocate()` relies on it.
- Key dedupe must stay a function of key contents only: sizing and persisting must agree on `e->size` (`ZEND_ASSERT` at the end of `persist_entry`).
- Every pointer inside a block points into the same block or to the arena's empty array (`ARENA_EMPTY`, in the header's `user` area): `nocopy_rebase` relies on it. Never to anything of the process (`zend_empty_array`, interned strings, function pointers: `pDestructor` is NULL): another process maps the arena at the same address but its own binary elsewhere (test 057 segfaults with `zend_empty_array`).
- Only the owner shard, under its lock, changes a segment's state; DEAD ends only in its `reclaim()`. So a walk over a segment (`expire_step`, `merge`) may run past the point where its last unlink made it DEAD: the memory stays until this lock is dropped.
- `seg_check()` is what moves SEALED -> DEAD; call it after any change of `live`/`pending`.
- `seg_carve()` sets an entry's size before `used` covers it, so `shard_repair` can always walk a segment. Entry sizes are multiples of 8 (slots hold offset/8).
- The slot table never holds more than `cap` (3/4) entries: probes and backward shifts rely on an empty slot ending every run.
- Tests that must see pressure use a small `nocopy.shm_size` (2M-4M, one shard). 015 (16M, 2 shards, 8 processes) and 018 (deletes shifting keys under readers) are the concurrency tests:
  run `./dev stress 30` after touching locking, reclamation or the slot table; a single pass catches races only sometimes. 018 fails reliably if `nocopy_find` stops retrying misses.
