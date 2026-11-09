# Architecture

The internals: data layout, locking, eviction, the read path, the Valkey tier and the invariants every change must keep.
Start with [How nocopy works](how-it-works.md) for the overview; this page is for people changing the C code.

## Layers

Layers, each using only the one below it:
- Arena (`arena/`, no PHP): entries are a header plus the caller's bytes. `arena/nocopy_arena.h` is its public API, `nocopy_*` its only names; the read path (`nocopy_find`, pins) is inline there. Every public call that writes takes its shard lock itself.
  Inside, `ar_*` modules, each a header (its API) and a unit; headers include only lower layers, `*_read.h` is a module's inline read side:
  `base.h` (offsets, errors, segment sets), `clock.h` (wall and monotonic time, injectable for tests), `proc.h`/`proc.c` (owner ids: pid + start time, locks that survive a dead holder, worker slots), `region.h`/`region.c` (the shm object: create, attach, collect),
  `entry.h` (the 48-byte header and its accessors), `pool*.h`/`pool.c` (segments: owners, generations, deps bits, pin bitmaps), `index*.h`/`index.c` (slot tables: probes, backward shift, `mseq`), `log.h`/`log.c` (a shard's segments: TTL buckets, allocation, expiry, reclaim, repair from the slots),
  `policy*.h`/`policy.c` (hot counter, ghost, merge), `atoms*.h`/`atoms.c` (strings stored once for the whole arena, below), `leases.h`/`leases.c` (single flight), `ctx.h` (a worker's handle), `shard.h`/`shard.c` (composes them: writes, deletes, updates, room-making, `repair` after a steal),
  `layout.h`/`layout.c` (what lies where; the only module that knows the header), `arena.c` (open, claim, stats). `./dev layers` (`scripts/check-layers.sh`, in `./dev ci`) checks the include order, that only `arena.c` and `layout.c` include `layout.h`, and that the layers above include only `arena/nocopy_arena.h` and use no `ar_` name.
- Values, interface `nocopy_value.h` (the block layout, entry flags, the key table, `nocopy_build`, copy-out, graphs out, serialize): `nocopy_value.c` puts PHP values in and out of an arena block (size, persist, rebase, copy-out, serialize, `nocopy_build`: size, `nocopy_put_begin`, persist, `nocopy_put_commit`); objects and shared references it hands to
  `nocopy_graph.c` (class fingerprints, the graph builder, persisting and rebuilding graphs, copy mode's templates). `nocopy_value_int.h` is shared by these two only: the walk states (`sizing`, `persist`, `rebase`), the graph builder, the graph markers and the calls each makes into the other.
- `nocopy_php.h`: what the PHP layers share: the module globals, the layout of the arena header's `user` area (`nocopy_shared`: the empty array every entry points to, the Valkey listener's state), the store API and the detour (`nocopy_detour_ops`).
- `nocopy_store.c`: the store API (`nocopy_store`/`fetch`/`drop`/`add_long`/`cas_long`/`remember`), the no-arena detour (`nocopy_no_arena`), leases.
- `nocopy_kept.c` (interface `nocopy_kept.h`): copy mode's kept copies. Callers make one call, `nocopy_kept_get` (`_graph` for templates; inline): it returns the kept copy, or the way to keep the copy they make now (`nocopy_kept_put`), and decides eligibility and admission itself.
- `nocopy.c`: `nocopy_*`, INI, the module, FrankenPHP's worker hook, the route (`route()` in MINIT, or the first RINIT when deferred). Pins only through the arena's calls (`nocopy_drop_idle`, `nocopy_request_end`).
- `nocopy_valkey.c` (optional, `--disable-nocopy-valkey` leaves it out): the Valkey tier. `nocopy_store.c` reaches it only through the `nocopy_vk_detour` table, set as `nocopy_detour` (without it, the table does not exist and the route never picks it); the tier uses the local store's pieces exported there (`nocopy_put_local`/`fetch_local`/`drop_local`) and the value layer's `serialize`/`unserialize`.
- `apcu/apcu.php` (PHP, not built): `apcu_*` over `nocopy_*` for code that cannot switch. The extension only registers `apc.enabled`/`apc.enable_cli` (when APCu is not loaded), which callers check before using `apcu_*`.
- Detour: each store-API function tests `nocopy_detour` once (NULL: the local fast path) and otherwise calls that table: Valkey or no arena. Positional initializers, so `-Wextra` flags a table missing a slot.
- No arena (`nocopy.shm_name` not set or not attached, `nocopy_attached()` false, the `nocopy_no_arena` table): every store-API function returns its miss or failure (stores still size and validate the value, so the caller's errors throw), `nocopy_remember` computes without keeping, `nocopy_tracked()` does not throw, RINIT claims no worker slot. `nocopy_shared_with()`: `none`.

`tests/c/stress.c` drives the arena directly from threads (plain and ThreadSanitizer, `./dev c`), shared counters bumped through `nocopy_update` among them (exact sum when none was evicted), values pointing at atoms while generations rotate; `tests/c/repair.c` and `tests/c/atoms.c` also inspect internals (`layout.h`). All pass a `nocopy_rebase_fn` in `nocopy_config`, the one callback the arena needs from its user (see Merge).

Everything lives in one POSIX shm object named by `nocopy.shm_name` (required), mapped in MINIT (php-fpm's master as root: in each worker's first RINIT, after it dropped to the pool's user, so root never maps memory workers write) at the same address in every process that attaches, forks included (pointers are valid everywhere); threads (ZTS) share it anyway (`nocopy_arena_open`, `arena/region.c`):
- Name `base.<8 hex>`: the layout id, a hash of the loaded `nocopy.so` (plus nocopy version and `ZEND_EXTENSION_BUILD_ID`), so processes share only when they run the same code. Mode 0600.
- Liveness: every process that maps it holds a shared `flock` on its object for life (fd kept open, inherited by forks, closed on exec). `LOCK_EX|LOCK_NB` succeeds only on an object no process maps.
- Create: `O_EXCL`, then `LOCK_SH` and a check that the name still refers to this object (`hold`: same `st_dev`/`st_ino`; someone may have removed it as unused in between),
  then `posix_fallocate` (Linux: a sparse tmpfs file past `/dev/shm`'s limit SIGBUSes on first touch), map at the first free address of `bases[]` (0x2000'0000'0000, else 0x10'0000'0000; `MAP_FIXED_NOREPLACE`), set up the header, store `magic` last (release).
- Attach: refuse an object not owned by our euid or open to group/others (`owned`: another user can create the name first in `/dev/shm`, and root ignores the mode). Map the header anywhere, wait up to 2 s for `magic`; while it is missing and the object is unused, its creator died: `remove_unused` unlinks it (under `LOCK_EX`, only if the name still refers to it) and we start over. Then check `abi`, `nshards`, `hold`, map at `base`.
  Test 079: root refuses nobody's object and its own at 0666.
  Test 060: 24 processes starting at once over an unfinished object all end in one arena; without the re-check in `hold`, most end up in orphans.
- `collect` (Linux, `/dev/shm`): other builds' objects we own under the same base are removed when unused; the first one in use is reported (MINIT warns, or the log when opened in RINIT: those processes do not share this cache). `nocopy_arena_close` unmaps the size it mapped, not the header's.
- No name, or any failure, leaves `nocopy_attached()` false: the process has no cache (below), never a private one, whose writes the others would not see.

## Layout

`[ header (shards, leases, pool and atoms headers, user area, workers) | slot tables | ghosts | segment records | pool: owners, meta, pin bitmaps | atoms: tables, doorkeeper, halves ][ segment 0 ][ segment 1 ] ...` (`ar_layout_plan`: each module sizes its part)
- Segment size = power of two, at least 64 KB, growing until there are at most 256 segments but not past 4 MB (`AR_SEG_MAX_SHIFT`): above 1G the count grows instead (8,192 at 32G), since an eviction round moves up to a segment's worth under the shard lock. Entries larger than half a segment take a run of contiguous segments.
- Segment table, owners and the workers' pin bitmaps (`pin_stride` words each, on their own cache lines) sit behind the slot tables, sized for `size >> shift` segments (`segs_off`, `owner_off`, `pins_off`); bitmap loops stop at `ar_seg_words`. Arenas up to 1G keep 4-word bitmaps.
- Entry = `nocopy_entry` header (hash, size, expiry, seq, 32-bit created, key length/offset, live/hot/flags, the user's 32-bit `user`: 48 bytes, `_Static_assert`ed) + the user's block. The PHP block is `zval` + key `zend_string` + value, written as opcache-style immutable structures
  (refcount 2, `IS_ARRAY_IMMUTABLE` / interned strings, hash precomputed), so a reader can hand the pointer straight to userland.
  Equal array keys, and equal strings of up to 64 bytes inside arrays and objects (`NOCOPY_VSTR_MAX`), are stored once per entry (rows of a result set share their column names and repeated values): a 256-slot table by hash (`NOCOPY_G(keys)`, `key_seen`) remembers this value's strings, keys and values alike; a miss overwrites the slot.
  Its decisions depend only on the sequence of string contents, so sizing (`nocopy_size_zval`, `size_str`) and persisting (`nocopy_persist_zval`, `persist_key`/`persist_vstr`) dedupe exactly alike and agree on the size. A string stored alone (the whole value) bypasses the table: it is reset for arrays and objects only.
  A table miss of a string up to 64 bytes then asks the atoms (below): sizing records each answer (the atom, or none) in walk order (`sizing.found`), and persisting replays that list instead of looking again, so both agree by construction.
  A deduped string longer than one byte gets refcount 3 (PHP never reads an interned string's refcount): copy-out (`copy_shared_str`) gives its uses one private copy, found in the same table by arena pointer. Atoms are stored that way (and with their hash), so they are never written after admission.
  A map's hash gets 2 slots per bucket, rounded up to a power of two (`map_mask`), not the table's 16-slot minimum: rechained on persist, as opcache compacts its arrays. Readers and `zend_array_dup` take the mask as they find it.
- Slot: 64 bits = `{ offset/8 (32 bit), ar_slot_tag(hash) }`; 0 empty. The tag is the hash times an odd constant, top 32 bits; its low bits are the home slot, all 32 the probe filter: a probe compares them first and only dereferences on a match.
  Not the raw hash: PHP's DJBX33A gives keys like `prefix.N` regular low bits, and as home slots they formed probe runs of hundreds at 3/4 fill (simulated: hit 215 / miss 1247 probes vs 2 / 8).

## Atoms: strings shared across entries

Short strings recur across entries (Drupal's cache tags and render keys: `languages:language_interface` in three quarters of its rows), so strings up to 64 bytes are also stored once for the whole arena, as atoms (`arena/atoms.c`; the arena stores byte strings, the value layer makes them zend_strings). Entries point at them; hits stay zero-copy.
- Area: two halves of `shm_size / 128` (the last part of the header), each with its own open-addressing table (a slot per 32 bytes, at most 3/4 full; slot = tag + offset/8, as the key slots) and one shared doorkeeper of tags. A record is `uint32 len, uint32 offset of the bytes, the blob` (for PHP: a zend_string header, the bytes, a NUL).
- Generations: each half holds one (`atom[h].state`); new atoms go to the current one, `atom_gen` (half `atom_gen & 1`). Lookups search the current generation only.
- Admission: a string sizing meets that is no atom (first in its value) writes its tag to the doorkeeper (direct-mapped, racy); the second sighting adds it (`nocopy_atom_add`) under the atom lock, a trylock: an admission can be skipped. Order: record bytes, `used`, then the slot (release): any state a dead holder leaves is one the next can build on, so a dead holder's lock is taken over (checked at most once a second) without repair. The put that admits does not use the atom yet (it was sized without it).
- Views: a writer announces its generation in its worker slot (`agen`, `nocopy_atom_begin`: store, full fence, check the half still holds that generation) from before sizing until after the commit (`nocopy_atom_end`); a nested write (a hook storing) shares the outer view. `put_begin` sets the entry's segment's bit for that half (`nocopy_seg.atoms`), under the shard lock, before the view ends; a merge move ORs `nocopy_rebase`'s answer (the halves the block points into) into the new segment. A segment's bits go only when it is freed (zeroed), which waits for every reader's pin, so a zero-copy value holding atoms keeps its half.
- Rotation: the current half full, `atom_rotate` (atom lock held) marks the other half `WIPING` (seq_cst, fence), looks for its generation in any worker's `agen` (a dead owner's is cleared), then, holding every shard lock (index order), in any segment's bits. Referenced: the mark is undone and no rotation is tried for a second (`atom_tried`). Else its table is zeroed and it becomes generation `atom_gen + 1`, the current one. A writer whose check sees `WIPING` retries with the new `atom_gen` (Dekker: either the rotation sees its announcement or it sees the mark).
  Bits change only under shard locks and a view ends only after its `put_begin` set its bit, so a view gone by the first look has its bit seen by the second; a rotation that dies anywhere leaves `WIPING` or a half-zeroed table that the next one repeats from the start.
- `nocopy_clear` leaves the atoms alone; it seals the shards' open segments, so with no pins every segment goes back to the pool and its bits with it.
- Ceiling: a generation stays while any segment that held an entry using it is not freed (a moved entry carries its bits along); while old entries live on, a full current half stores new strings per entry again. Tests: 086 (a zero-copy reader keeps its half past a clear; unpinned, the next fill rotates), `tests/c/atoms.c` (every blocking and dead-holder case), `tests/c/stress.c`.

## Shards and the segment pool

The key hash (Fibonacci mix of the high bits) picks one of up to 8 shards. Shards exist only while each one keeps >= 8 MB and >= 16 segments: 1 shard up to 8 MB, 2 at 16 MB, 4 at 32 MB (default), 8 from 64 MB.
A shard owns, for the arena's life: its lock (alone on its cache line), a fixed slot table, one open segment per TTL bucket, its expiry cursor, live bytes and `nlive`; and, while it holds them, the segments it took from the pool.
Every write takes only its key's shard lock and only touches that shard's slots and segments.
- Pool: `owner[s]` (header, one byte per segment) is the shard index + 1, 0 while the segment is free. A shard claims with a CAS 0 -> its id (under its own lock) and releases with a store of 0 after zeroing the segment (`ar_pool_give`, from `ar_log_reclaim`), so a segment changes shard only through the pool, zeroed, and its old shard never walks it again.
  The shard's `own` bitmap and `nown` index its segments; `owner[]` is the truth (`repair` rebuilds both from it and returns segments claimed but never set up, or freed halfway).
- `ar_pool_take` scans `owner[]` from the shard's home range (`idx * nsegs / nshards`): balanced shards stay contiguous, as with fixed slices. A plain take leaves one free segment per shard, for merges (`reserve`).
- Balance: a shard below 15/16 of its share (`nsegs / nshards`) that finds the pool dry has the shard holding the most evict once (`rebalance`: drop own lock, lock that one, `reclaim` + `ar_policy_merge`, unlock, relock) before it merges its own. Above its share it merges its own, as before.
  So a hot shard borrows what others do not use and gives it back when they need it; idle memory is never taken from a shard that is not over its share.
Price: an entry must still fit in a shard's share (larger ones are refused; fragmentation would make bigger runs fail anyway).

Locks: test-and-test-and-set, inline fast path (`ar_lock_acquire`), slow path `ar_lock_wait` yields after 128 spins. The lock word is the holder's owner id = pid | process start time << 32.
A waiter that has yielded ~1000 times checks `ar_owner_dead(holder)` (gone, a zombie, or the pid now belongs to a newer process: start time differs; Linux `/proc/<pid>/stat`, macOS `sysctl(KERN_PROC_PID)`)
and takes the lock over with a CAS (`lock_steals` in `nocopy_stats()`), then runs `repair()` (`shard.c`): the slot table is the truth (it only changes by single stores), everything else is rebuilt from it.
An odd `mseq` (a delete died mid-shift: every miss in the shard would retry forever) is made even and the key it left in two slots loses its second one; each segment's `live` comes from the entries slots reach (an entry unlinked halfway stops counting),
`pending` from the worker slots' `wip` records (an abandoned allocation stops holding its segment), states from the runs (a run taken or freed halfway, an open segment never installed). O(slots + entries) under the lock, only after a steal.
A writer that dies filling a big entry outside the lock holds no lock: before unlocking it records the entry in its worker slot (its `wip`, which changes only under that entry's shard lock), and the entry is aborted when the slot is claimed again,
by `nocopy_clear`, or when an allocation fails (`wip_reap`, at most once a second per shard). Tests: `tests/c/repair.c` (each half-done state, deterministic), 037 (40 kill -9s of a writer mid inline/big put/delete).
Robust pthread mutexes would let the kernel report a dead holder, but measured on Linux they are 15% slower uncontended and 2-8x slower with 2-16 contending processes (futex sleeps, also with a trylock spin in front). Not used.

## Slot tables: fixed, backward-shift deletes

Sized once: ~2 slots per `nocopy.entries_hint` and at least one per 256 bytes of arena (3% of it), split evenly over the shards. A shard holds at most 3/4 of its slots (`cap`);
an insert into a full table evicts (merge, below) like a full memory does. So a cache of very small entries evicts before its memory is full: raise `nocopy.entries_hint` for that.
Deletes use backward shift (linear probing without tombstones): later entries of the probe run move back into the hole, so probe runs never fill up with tombstones.
A reader that probes past a key while a delete moves it back would miss it: deletes that move anything bump the shard's `mseq` (odd while moving, its own cache line),
and `nocopy_find` reads `mseq` before probing and treats a miss as real only if it is unchanged and even (seqlock); hits need no check (pin + slot re-check below).

## Writes

A write is a `nocopy_put`: `nocopy_put_begin` locks the key's shard, allocates (bump-allocate in the shard's open segment for the entry's TTL bucket, or take a fresh one / a run), sets the header and the segment's atom bits;
the caller (`nocopy_build()` in nocopy_value.c: size the value, `persist_entry`) fills key and payload; `nocopy_put_commit` locates the slot, publishes with a release store, marks the old entry dead and unlocks. `nocopy_build` frees what sizing kept only after the commit (it may run destructors).
Blocks up to `NOCOPY_INLINE_PUT` (2 KB) are filled under the lock (one critical section); larger ones are filled with the lock dropped and recorded as the worker's `wip` meanwhile (see Locks). The caller never sees the lock.
Per segment: `used` (bytes appended), `live`, `pending` (allocated, unpublished). A sealed segment with `live==0 && pending==0` becomes DEAD (`ar_log_check`); `reclaim()` frees DEAD segments no worker pins, under the shard lock.
TTL buckets (Segcache): one open segment per bucket (ttl 0, < 8 s, < 64 s, < 512 s, < 4096 s, < 32768 s, longer); each segment keeps `max_exp`. Every write runs `ar_log_expire_step()` on its shard: a cursor walks the entries of
the shard's segments with `max_exp <= now`, at most 64 per write, and unlinks them; after a full pass with nothing to do it idles until the next second. `exp_gen` vs the segment's `gen` (bumped on every free) keeps a cursor from resuming mid-entry in a reused segment.
When an allocation fails: `reclaim()` -> `ar_log_expire_step()` with a large budget -> `ar_policy_merge()` repeatedly.

## Merge (Segcache-style): compaction and eviction in one

`ar_policy_merge()` picks up to 4 victims among the shard's sealed segments (`pick_sparse`, else `pick_oldest`; then `move_survivors`, `evict_rest`):
- compaction if the sparsest sealed ones fit together into one segment (at least two), or the sparsest alone is at most 3/4 live (never a segment holding one entry over half a segment: moving it takes a whole segment again, so it frees nothing): every live entry moves forward (`relocate`: into its segment's TTL bucket, slot repointed), nothing is lost;
- else 4 victims among the 32 oldest (`born`, the shard's `last_seq` when the segment was taken): the oldest and the 3 with the fewest `reads` (per segment: the sum of its live entries' `2^hot - 1`, adjusted at birth, death and by readers on each `hot` bump; racy, signed, reset when taken); of the entries read since they were written or last moved, the most-read move forward, up to one segment's worth, the rest is evicted (`expunges` counts the round, before the unlinks; the shard's `evicted` / `expired` count the entries).
  Ranking: bytes per `hot` level are summed over the victims; levels that fit the budget all move, the next level down fills what is left first come. A moved copy keeps half its count (aging), so it must be read again to stay on top. A big entry's run is evicted when oldest, unless it was read: then it goes to the back once (count halved).
Ghost (S3-FIFO): per shard, one 32-bit tag per slot (in the header after the slot tables), direct-mapped by a Fibonacci mix of the hash below the shard's bits. An eviction writes the key's tag;
a new key whose tag is there starts at `hot = 2` (survives two merges unread; `ghost_hits`). Keys used once never come back, so only recurring keys whose reuse distance exceeds the cache get the extension.
`hot` is a byte in the entry header, a Morris-style read counter (0-15): the first read sets 1, later reads add 1 with probability 1/2^hot (per-worker xorshift), so a hot key writes its cache line ~log2(reads) times between merges and it stays shared otherwise (exact per-entry hit counters would write on every read).
Victims that readers pin die but stay allocated until unpinned; the next round moves on.
Moving an entry is a `memcpy` of its block plus the user's `nocopy_rebase(e, from)`: nocopy_value.c walks the value (nocopy_graph.c a graph's classes and nodes) and shifts every pointer that pointed into the old block (arrays' `arData`, bucket keys, strings, nested arrays) by the distance moved; `ARENA_EMPTY` and atoms stay, and the halves of the atoms met are returned for the new segment's bits.
Moved value strings get `h = 0` (a reader may have been hashing the old copy during the memcpy). Nothing is re-persisted, so the moved block has the old size by construction.

## Reads (lock-free) and pins

`nocopy_find()`: load the slot, compare tag, then `ar_pin(entry)` = publish the entry's segment bit in this worker's bitmap (seq_cst store + fence), re-check the slot is unchanged, only then dereference.
Slot tables never move, so readers use a process-local copy of their addresses (`ar_ix`) and touch no header cache line that writers write.
Bits set during a call outlive it as idle pins (re-pinning costs a full fence per call). They are dropped at a call end once a writer bumps `unpin_gen` (`reclaim()` found a dead segment still pinned) or past `nsegs/16` idle pins, by a writer before it allocates (its own pins would block its reclaim), before a FrankenPHP worker waits for a request, and at request end. Ceiling: a process idle between calls holds up to `nsegs/16` segments. Not dropped early are segments a zero-copy pointer escaped from (`nocopy_keep`): those stay until post-deactivate (not RSHUTDOWN: session and other built-ins run their RSHUTDOWN after ours and still serialize user values). There is no earlier release: userland can keep any value until the request ends. So a request keeps at most `nsegs/4` (`nocopy_keep` says when): past that its further fetches copy (`zero_copy` off until RINIT), and its kept pins stop growing.
`nocopy_each()` (`nocopy_keys`) walks every shard's slots the same way, lock-free: pin, re-check the slot, then read; what it found stays pinned until `borrow_end`. A key a concurrent delete shifts backwards may be missed.
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
- copy: `nocopy_copy_into` = `zend_array_dup` (one memcpy of an immutable array) + `nocopy_unshare_zval` over its values (repoint arena strings/keys). The same key table maps a deduped arena key to its first private copy, which later rows share (refcounted).
  Kept copies (`nocopy_kept`, `nocopy.local_size`): a 4-way set-associative table (1024 sets of 4) in request memory, by entry address, holds `{entry, seq, size, copy}`. A fetch whose entry has the same address and `seq` returns the kept copy (refcount + 1; COW protects it); otherwise it copies, and keeps the copy only if the slot's `seen` is this entry (second sighting, TinyLFU's doorkeeper), else just records it in `seen`. A miss takes the way with neither a copy nor a pending `seen`, else one without a copy, else one the SIEVE hand found unused (taking any empty way first let new keys overwrite each other's `seen`, and a loop kept nothing). Keeping every copy made fetches of 100k distinct strings 37 -> 64 ns (freeing cold evictees).
  `(address, seq)` is one version: `seq` is microseconds `<< 3 | shard` and only grows per shard, so a reused address (also in another shard, segments move through the pool) has another `seq`; a merge move changes the address (one re-copy). Integers changed in place (`nocopy_inc`) are never kept. Over budget a SIEVE hand (`local_hand`) clears `visited` bits (set on each kept-copy hit) and drops unvisited copies until the new one fits; freed in RSHUTDOWN (later fetches copy without keeping).
- graph entries (`ENTRY_GRAPH`): the value is an `nc_graph` in the block: classes `{name, lowercase name, fingerprint, slot count}`, shared references' values, objects as nodes, the hooks list, the root.
  A node with `dyn` set has its dynamic properties as an array after the slots (`stdClass`: the whole properties table); on fetch each object gets a private copy (`graph_dyn`: the engine's own `(object)` cast still copies immutable tables), merged into `rebuild_object_properties_internal()` when the class also has slots.
  A class's kind is in its fingerprint's low bits: slots (raw property slots), slots + `__wakeup`, sleep (the slots `__sleep()` names, `NC_DEFAULT` for the class default in the others; also with `__wakeup`), data (`props[0]`: `__serialize()`'s array), enum (`props[0]`: the case name) or date (`DateTime`/`DateTimeImmutable`, not subclasses: timestamp, microseconds + zone type + offset, zone ID or abbreviation; rebuilt with ext/date's exported calls, as timelib's are not exported, and only an initialized date with a zone and no dynamic properties, else the value is serialized). Hooks (`__wakeup`, `__unserialize`) run after the whole value is built, inner objects first like `unserialize()`; `graph_build` stops reading the graph before they run (user code may drop a template).
  Sizing runs no user code: an object whose `__serialize`/`__sleep` has not run is recorded (`ghook`, held, found by object: linear for up to 4, else a map by handle) and its contents skipped; after the walk the hooks run (`nocopy_graph_b_hooks`, outer first, then those their results reveal), and sizing starts over until none is pending (at most `NOCOPY_MAX_DEPTH` passes, else the value is serialized).
  So no hook changes what a walk is iterating, and the last pass sees exactly what persisting will (test 074: a hook that replaces an object already walked broke the block size).
  In a graph an object is `NC_OREF` (index), a reference held more than once `NC_RREF` (index into the graph's `refs`, its value; once: stored as the value) and an array with either below it `NC_SPINE` (persisted, but copied on fetch); arrays without them are ordinary immutable arrays. Graph values are plain (at most `IS_ARRAY`) or a marker, so one compare finds the fast path.
  `nocopy_graph_out` resolves every class first (class table, else autoload: user code), checks its fingerprint (slot names, visibility, readonly, types; `class_fp`, cached per request by class entry), then `zend_objects_new` (no constructor) and the slots copied in. Zero-copy: strings and object-free arrays are the arena's; copy: copied out like any value, and from the second fetch of the same version on built from a template kept in `nocopy_kept` (`IS_PTR`: the graph's bytes with private refcounted values and resolved classes; freed by `nocopy_kept.c`). Any fingerprint mismatch or missing class is a miss.
- serialized entries (other objects, shared refs): `cache_unserialize` with a private unserialize state.

## Single-flight and early refresh (`nocopy_remember`)

A lease is not a cache entry: each shard has 32 fixed `ar_lease` slots `{key hash, owner id, worker slot, until}` in the header, read and written under that shard's lock (the key's own shard).
`nocopy_lease_take` returns TAKEN, HELD, MINE (recursion: our own callback asks for its key) or FULL (32 concurrent computes in the shard: compute without one). An expired lease (30 s) is free;
a holder found dead (`ar_owner_dead`, checked outside the lock since it reads /proc) is replaced on a second pass only if it still holds, and `nocopy_lease_drop` clears a lease only if this worker slot still holds it, so nobody drops a lease another worker retook.
Leases are keyed by the 64-bit hash: two keys with the same hash share one (one waits for the other, then computes). Waiters poll (50 us doubling to 5 ms) for the value; meanwhile a local (not Valkey) waiter with `beta > 0` takes an expired entry (`nocopy_find_ex(stale)`, `nocopy_stale_ok`: expired less than its TTL ago). Held leases are listed in module globals and dropped in post-deactivate if a callback bailed out.
`nocopy_stats()['leases']` counts the held ones; `entries` and `nocopy_clear` never see them.
The callback's run time (ms) is kept in the header's `user` field (the arena copies it when merge moves the entry, test 036). XFetch on a hit: refresh when `now - delta * beta * ln(rand) >= expires`; the refresher needs the lease, the rest return the current value.

## Counters and INI

`nocopy_inc`/`nocopy_cas` go through `nocopy_add_long`/`nocopy_cas_long`: `nocopy_update` runs their callback on the live entry under the shard lock, which modifies the integer in place with relaxed atomics (readers copy that word). Deletes: `nocopy_delete`. The lock is not reentrant, so a callback never calls back into the arena. `nocopy_clear` takes all shard locks in index order.
INI values set in MINIT (`nocopy.serializer` in effect) go through `ini_put()`, never `zend_alter_ini_entry`: that one records the entry in `EG(modified_ini_directives)`, emalloc'ed memory that `php_module_startup` frees at its end, and the next `ini_set()` of a new directive wrote into it (segfault in Symfony's boot).

## Valkey tier (`nocopy_valkey.c`)

`nocopy.valkey` makes Valkey the cache of record; the arena holds local copies. `nocopy_store.c` hands `nocopy_store`/`fetch`/`drop`/`add_long`/`cas_long`, the multi variants,
`compute` and `nocopy_clear_all` to `nocopy_vk_detour` (only with an arena; without one the route picks `nocopy_no_arena`); `nocopy_valkey.c` owns the routing, the connection and every command.
- Connections are libvalkey's (vendored in `third_party/libvalkey`, `scripts/vendor-libvalkey.sh`; compiled with `-w`, `-U_GNU_SOURCE` and its API hidden): blocking `valkeyContext`s with `nocopy.valkey_timeout` for connect and every read and write,
  `VALKEY_OPT_NO_PUSH_AUTOFREE` so pushes come back as replies and are applied here. Commands are built as RESP by hand (the key prefix without a copy) and queued with `valkeyAppendFormattedCommand`.
- TLS (`tls://`, `rediss://`, `HAVE_NOCOPY_TLS`): our `SSL_CTX` from MINIT, shared by forks and threads; our `SSL` (SNI, host or IP verified) handed to `valkeyInitiateTLS`. libvalkey frees it without close_notify, so a forked child leaves the parent's session intact.
  Its socket BIO writes without `MSG_NOSIGNAL`: PHP's SAPIs (CLI, php-fpm, FrankenPHP) ignore SIGPIPE, the listener does so itself, macOS sockets get `SO_NOSIGPIPE`. The listener polls with 0 ms while `SSL_pending`. Errors leave nothing in OpenSSL's per-thread queue (PHP's openssl functions read it).
- A command connection per worker (`NOCOPY_G(vk)`), lazy. Handshake: `HELLO 3 [AUTH] SETNAME nocopy`, `SUBSCRIBE` to an idle channel (exempt from Valkey's `timeout`); its reply is read there, and a refusal (`NOPERM`, no channel permission) only loses the exemption.
  No tracking: a reconnect needs no clear. Any error or timeout closes it (the reply stream would be out of step); a new attempt waits 0.5-1.5 s.
- One listener per arena: a process of its own (double fork, `setsid`, signals reset, every inherited fd closed, stdio on /dev/null, `nocopy-valkey`), running only C:
  one connection with `CLIENT TRACKING ON BCAST PREFIX p NOLOOP`, `poll` at 100 ms, applies pushes, `PING` every second (no answer within max(1 s, timeout): reconnect).
  In the header's user area (`nocopy_shared`; the arena itself knows nothing of Valkey): `vk_beat` (monotonic ms, every poll round), `vk_pid`, `vk_live` (connected). Every (re)connect bumps `inval_gen` and clears the arena: what it missed is unknown.
- `nocopy_vk_ready()` once per request (`synced`, reset in RINIT and per FrankenPHP request): command connection up, and the listener live (`vk_live` and a beat under 1 s old).
  No beat for 1 s: the first worker to swap `vk_beat` (CAS) spawns a new listener; workers wait up to the timeout for `vk_live`; after a wait that failed, a worker only checks, without waiting, for 0.5 s. A listener whose `vk_pid` is not its own exits. While not ready nothing local is served.
- Lifetime: the listener drops its shared flock (so the segment can still be collected) and exits when `/dev/shm/<name>` no longer is its object (`nocopy_arena_current`, `stat`: fpm workers run as another user than the segment's owner).
- Push `invalidate [keys]`: `inval_gen++` (seq_cst) first, then `nocopy_drop_local` per key; a null list (FLUSHALL) clears the arena.
- Fill guard: `vk_fetch` reads `inval_gen` before its `GET` and drops the copy it just stored if the counter moved. The listener bumps before it drops, so either the filler sees the bump or the drop comes after the store.
- Writes: local first, then `SET` (an invalidation from elsewhere can only remove our copy, never be overtaken by it). `add`: `SET NX` first, local after. The listener gets our own write pushed too and drops the fresh copy: one refetch per write per server.
- Encoding: integers as decimal (for `INCRBY`); else `0x00, flags, int64 expires, uint32 compute ms` + the serialized value. `PXAT` = (expires + 1) s.

## Invariants worth remembering

- The arena (`nocopy_arena.h`) knows nothing of PHP or Valkey. The layers above change its state only through its calls (pins: `nocopy_borrow_begin`/`end`, `keep`, `drop_idle`, `request_end`; counters: `nocopy_count`), never by writing its fields; their own shared state lives in the header's `user` area (`nocopy_shared`).
- Never allocate (emalloc, zvals) while holding a shard lock: an OOM bailout (PHP <= 8.6) or the hard-limit `_exit` (8.7+) would leave it locked for every worker. `nocopy_rebase` runs under the lock: it only rewrites pointers.
- No user code while sizing, persisting or holding a shard lock: hooks run between sizing passes, freeing what sizing kept after the commit.
- Never take a second shard lock while holding one, except `nocopy_clear` (all, in index order). `rebalance` drops its own first: `alloc_relocking` may unlock and relock, so nothing before it in a write may rely on shard state.
- A segment changes shard only through the pool: zeroed, `owner[s] = 0` (release), claimed by CAS. Walk a shard's segments by `own` (`AR_LOG_SEGS`), never by index range.
- Arena strings are `IS_STR_INTERNED` but never `IS_STR_PERMANENT`: that flag promises process lifetime and PCRE then keeps the pointer as a cache key.
- Only keys carry a hash. Value strings are stored with `h == 0`; PHP hashes them on first use as a key and writes `h` into the arena, a benign idempotent race.
- Never dereference an entry before `ar_pin` + re-check (`-DNOCOPY_POISON`, set by tests/c, fills freed segments with 0x5A to catch this).
- A live entry is always reachable from exactly one slot; `relocate()` relies on it.
- String dedupe must stay a function of string contents and walk order only: sizing and persisting must agree on `e->size` (`ZEND_ASSERT` at the end of `persist_entry`). Atoms are looked up only while sizing; persisting replays the answers in the same order (asserted too).
- Every pointer inside a block points into the same block, to the arena's empty array (`ARENA_EMPTY`, in the header's `user` area) or to an atom: `nocopy_rebase` relies on it.
- An entry may point at atoms only of the generation its writer's view announced, and only with its segment's bit set before the view ends (`nocopy_put_req.deps`). Never write to an atom after admission. Never to anything of the process (`zend_empty_array`, interned strings, function pointers: `pDestructor` is NULL): another process maps the arena at the same address but its own binary elsewhere (test 057 segfaults with `zend_empty_array`).
- Only the owner shard, under its lock, changes a segment's state; DEAD ends only in its `reclaim()`. So a walk over a segment (`expire_step`, `merge`) may run past the point where its last unlink made it DEAD: the memory stays until this lock is dropped.
- `ar_log_check()` is what moves SEALED -> DEAD; call it after any change of `live`/`pending`.
- `ar_log_carve()` sets an entry's size before `used` covers it, so `repair` can always walk a segment. Entry sizes are multiples of 8 (slots hold offset/8).
- The slot table never holds more than `cap` (3/4) entries: probes and backward shifts rely on an empty slot ending every run.
- Tests that must see pressure use a small `nocopy.shm_size` (2M-4M, one shard). 015 (16M, 2 shards, 8 processes) and 018 (deletes shifting keys under readers) are the concurrency tests:
  run `./dev stress 30` after touching locking, reclamation or the slot table; a single pass catches races only sometimes. 018 fails reliably if `nocopy_find` stops retrying misses.
