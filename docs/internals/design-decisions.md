# Design log: decisions and rejected ideas

Why nocopy is built the way it is, with every measurement behind each choice, and what was tried and dropped. For
contributors (and their agents): read this before proposing a change in these areas, since the obvious alternative has
often been measured already. The user-facing summary is [Design decisions](../explanation/design-decisions.md).

Numbers are from Linux in Docker on arm64 unless noted ([benchmarks](benchmarks.md)). History is in `git log`.

## Memory lifetime

**Per-worker pin bitmaps over segments.** A worker marks the segments it reads from; a segment is reused only when no
worker pins it. The first design, an epoch plus a per-entry retire list, piled up garbage; the old "zero-copy write is
50-100x slower" results came from it.

**Pins released only at request end.** Userland may keep any fetched value until then, so there is no earlier safe point.
Long-running processes therefore use copy mode.

**A request keeps at most a quarter of the segments; past that its fetches copy.** Unbounded, one zero-copy request that read and
wrote across a full arena pinned every segment and the cache collapsed for all workers (32 MB, random 500 B overwrites in one
php -S request: 207 of 207 segments pinned, 203 dead, 27,962 entries -> 867, hit 90% -> 10%). With the cap: 27.5k entries, 88.7%
hit (copy mode: 90.1%). Not gated on pressure (`unpin_gen`): the first writer to meet a pinned dead segment came when the request
already kept ~185 of 207. Short Drupal run (`nocopy-all`, default 32 MB, 243 segments, cap 60), 28,640 requests: kept p50 5,
p99 41, max 61; 193 (0.7%) reached the cap and copied the rest of their fetches.

**The budget is arena-wide, and a request older than `nocopy.zero_copy_seconds` (3) copies** (was: a quarter per request).
The count is the union of
the live workers' published keep bitmaps, looked at on a request's first and every 8th new keep: a sum (one atomic add per
keep, the review's proposal) counts a hot segment once per request holding it and would push ordinary concurrent traffic into
copy mode. Repro (32 MB, 236 segments, 30k keys of ~600 B, 4 CLI workers 90/10 get/put for 8 s while N `php -S` requests
each read every key from their own starting point and hold them): hit ratio of the CLI workers, old -> new: N=1 83.2 -> 82.7%,
N=2 72.6 -> 82.2%, N=4 49.3 -> 81.2% (77.9% on a rerun), N=8 35.7 -> 54.6% (50.9%). At N=8 the rest is idle transient pins:
a request asleep holds up to segments/16 it never drops (looking at the union on every keep instead: 58.3%, within noise).
`./dev ab HEAD 7 read` within noise (COW and object rows rerun twice: ±5%). Price: a second bitmap per worker slot (packed:
one 64-byte line each cost a 4 MB arena one segment, test 029 lost a key). Micro benches set `nocopy.zero_copy_seconds=0`:
they run as one long `php -S` request. Tests 096, 097.

## Sharing

**`nocopy.shm_name` is required; without it there is no cache.** Unset or not attachable means a startup warning and no
cache, never a private one: a private cache splits invalidation silently ([Sharing](../explanation/sharing.md#all-or-nothing)).

**One off state** (was four behaviours: misses, `nocopy_clear()` answering `true`, an uncaught `Error` without a worker
slot, silence on ignored settings):
no name, no attach and no worker slot all route to the no-arena table, which misses and fails every write and
the clear, with the reason in `nocopy_stats()['off']`. The route moved into the module globals (per thread: a ZTS thread
can lack a slot while the others have one): `./dev ab HEAD 7 read write` within noise on NTS (rows ±6%, with signs
flipping across groups); ZTS not measured. `nocopy_clear()` with RESP unreachable is `false` too: the other servers
keep their copies.
**No default name:** two sites of one user would share keys.

**Liveness from kernel file locks** (was: pid + start time from `/proc/<pid>/stat`, where every failure to read meant dead): a live
holder in another PID namespace or at its fd limit was taken for dead and its lock stolen.
Each process holds an OFD lock on its process slot's byte of the shm object; a holder is dead when nobody holds the byte or its slot's generation moved on, and a query that fails means alive.
Price: one descriptor per process, a 1024-process limit at attach. The lock fast path is unchanged; the query runs only where `/proc` was read before.
macOS keeps pid + start time (`sysctl`, no fd, no namespaces; no OFD locks there).

**RESP: one listener process per arena** holds the only tracking connection: 16 connections and 16 copies of every push
per server became 1. The writer's own server refetches its write once, since the listener cannot tell own writes apart.

**RESP connections from vendored libvalkey** (`third_party/libvalkey`, 0.5.0), replacing our own socket, TLS BIO and RESP3 parser
(~600 lines and a fuzzer): Cluster, when it comes, is libvalkey's `cluster.c` rather than ours. Vendored because no distro or Homebrew
packages it and PIE builds from the release archive (submodules are left out). `bench/` has no RESP A/B; a put/get/inc/get_multiple loop
over loopback (scratch script, 9 interleaved rounds, rounds where both builds ran at the VM's fast speed): put 10.3-11.4 -> 11.5-12.0 µs
(each reply is a malloc'd tree), get 3.9-4.0 -> 3.7-4.0, inc 11.8-13.8 -> 12.5-13.0, get_multiple of 100 58-75 -> 52-71. Price: ~7k lines
of C in the repo, updated with `scripts/vendor-libvalkey.sh <tag>`.

**RESP tier behind a seam.** `nocopy_store.c` reaches it only through `nocopy_detour`, a table of the store ops whose other implementation is no arena; `--disable-nocopy-resp` compiles the RESP table
out, so the core builds and ships without it. CI runs the suite both ways. Default stays on.
The table replaced ten hand-written `detour`/`nocopy_vk_on`/`nocopy_a` branches; the local path only tests a pointer instead of a bool (`./dev ab main 7 read`: fetch int 10.96 -> 10.69 ns, 100k distinct keys 27.0 -> 26.9, within noise).

**The arena takes its shard locks itself.** `nocopy_delete` and `nocopy_update(fn)` replaced the store's own lock/lookup/remove; the lock is internal to the arena. `inc` pays one indirect call: `./dev ab main 7 write contend` 10.59 -> 10.49 ns, within noise. Its lookup stays out of line: `locate` inlined into `nocopy_update` made `inc` 10% slower (`./dev ab main 7 write`: 13.0 -> 14.4 ns; back to 12.9 with a noinline `lookup`).

**Kept copies as their own module** (`nocopy_kept.c|h`, one call order for callers, accepted at a layout cost): `./dev ab main 9 read` fetch miss 8.80 -> 9.08 / 8.81 -> 9.09 ns (copy / zero), string 100B copy 17.9 -> 18.7, the rest within ±1.5%. Zero-copy and misses run none of its code; `noinline` on `put`, the other include order and the old call sequence each only moved the loss to other rows, so it is the unity TU's layout, not work.

## Hashing and slots

**Slot home from a mixed hash.** PHP's DJBX33A gives keys like `prefix.N` regular low bits; as home slots they clustered.
On one shard at 73% fill: hit 28.8 -> 17.8 ns, miss 58.3 -> 15.2 ns, delete+put 107 -> 29 ns. Price: `fetch int, 100k
distinct keys` 22.1 -> 26.8 ns (keys spread over more cache lines). Test 018 picks keys homed in the first half to keep
its race window.

**Unseeded key hash, accepted:** keys are hashed with `zend_string_hash_val()` (DJBX33A, no seed), so colliding keys are
easy to build (`Ez`/`FY` pairs). 65,536 of them in a 256M arena: put 4.9 s, get 5.8 s (normal keys 0.008 / 0.002 s), each
put probing under its shard lock; unrelated misses on a 65k cluster 0.074 s per 50k. A per-arena seeded XXH3 removed it
(0.008 / 0.004 s) but cost every keyed call 1.5-2 ns, PHP's hash being cached in the string: `./dev ab` fetch int 10.9 ->
12.6 ns, string 100B 19.0 -> 21.9, `get_multiple` 10 keys 111 -> 139, inc 10.3 -> 11.8, parallel fetches -8..-13%. APCu has
the same weakness. Callers with keys from user input hash them first ([functions](../reference/functions.md)).

**Fixed slot tables, not growable:** no stall while growing. Price: many tiny entries evict by count before memory is full;
raise `nocopy.entries_hint`.

**Keep `nocopy.entries_hint`** (see rejected ideas below for dropping it).

## Copy mode

**The fetch mode follows the SAPI, then the request's age and the arena's pin budget** (below); only the age has a setting, for long requests that are meant to be. **Before: the SAPI alone, no setting.** `nocopy.request_scope` (`auto`/`1`/`0`) only let users get it wrong:
`1` under a long CLI worker pinned segments until it exited, `0` under php-fpm copied for nothing. Nobody needed zero-copy CLI
scripts. Price: none on the fetch path (same code). Tests and `bench.sh` get zero-copy as `php -S` requests (`tests/serve.inc`); `fpm-bench`'s copy row, FrankenPHP's forced run and the Drupal suite's zero-copy mode went (phpunit is CLI).

**Kept copies** (`nocopy.local_size`, 8M default): 1k rows 68 µs -> 12.8 ns, 10k rows 711 µs -> 12.8 ns, 10 KB string
182 -> 12.8 ns; small values at most 0.8 ns slower. **Kept on the second fetch only** (a doorkeeper): keeping every copy
made 100k distinct strings 37 -> 64 ns. Over budget, a SIEVE hand evicts copies not used since it last passed (was: a full flush). `bench/local.php`, 3000 arrays
of 20 rows under Zipf: local_size 4M 320-371 -> 233-242 ns, 1M 423-429 -> 333-366 ns; within budget and on loops unchanged.
**4-way set-associative** (was direct-mapped, 4096 slots either way): colliding keys evicted each other with budget to spare. `bench/local.php`
3000 arrays of 20 rows (~9 MB), local_size 32M, 5 interleaved runs: Zipf 103-135 -> 47-53 ns (zero-copy: 49-66), loop 352-574 -> 94-108 ns.
Price: kept-copy hits +0.6-2 ns (`ab.sh read`, near its noise); a loop over more than the budget 788-1490 -> 1474-1711 ns (more copies kept,
then freed unused). Copy mode is what long-running workers use, so this is their part of zero-copy: give them a `local_size` that holds their working set.
**The budget counts the copy's heap bytes** (`zend_memory_usage()` around the copy, only when it is kept; was: the entry's size). Since
arrays are shared arena-wide an entry can be a pointer to a shared array, a few dozen bytes for a copy of any size, so `local_size` bounded
nothing: 64 keys holding one 100-row array, rewritten while a zero-copy request past its pin budget reads them (300k gets): 101 MB -> 9 MB of
request memory, and `./dev tail 8 10 contend` in zero-copy died at `memory_limit` (128M) after ~3 s. Same bench, copy mode, one run each:
gets 218.6k -> 334.3k ops/s (useless copies no longer kept and freed), puts unchanged. A copy larger than the budget is not kept. Test 106.
`./dev ab HEAD 5 read`: within noise (kept-copy rows rerun at 9 reps: -1.4 to +2.5%).

**Deduped value strings get one private copy per copy-out, not one per use** (persisting marks them with refcount 3; the
key table finds the first copy by arena pointer, as for keys). Cold copies (`local_size=0`, scratch A/B, best of 7): 1k rows with
4 repeated strings each 78.0 -> 69.7 µs (+12%), 10 such rows 764 -> 742 ns, 100 objects with 2 repeated strings 4.81 -> 4.29 µs
(+12%); 1k rows of unique strings -0.6% (HEAD against itself ±0.6%). Asking the table for every string (no mark) cost unique rows
-5%, marking 1-char strings too -2.5%. `./dev ab HEAD 5 read write`: within noise (kept copies and zero-copy never copy out). A merge move keeps
their hash (other moved strings get 0, as a reader may be hashing the old copy): it was set before writing, so no reader writes it,
and the table finds them by it; zeroed, every moved one landed in slot 0 and its uses got a copy each again.

**Copy-out of `""` and 1-char strings uses PHP's interned ones** (`ZVAL_STRINGL_FAST`): no allocation. A cold copy of
`bench/run.php`'s 1k rows (two 1-char strings each, `local_size=0`) 69 -> 59 µs.

## Writes

Found with cachegrind (per-op cost from runs of n and 2n ops, `cg_diff`). **`persist_array` takes the next free index
from the loop it already runs** (was a second pass over every array, ~10% of a put), and **its key dedupe remembers the
source key**, so repeated keys match by pointer (was a `memcmp` against the persisted copy for every key). Store array
10 / 1k / 1k unserialized-key rows: 616 -> 520 ns, 59.2 -> 49.5 µs, 65.7 -> 53.8 µs. **`nocopy_get_multiple` sizes
its result up front**: no rehash, 10 keys 122 -> 109 ns.

**`nc_same_array` walks both arrays in step when neither has holes** (same layout, `nNumUsed == count`): no per-element
`next_elem` bookkeeping. `./dev cg put_arr`: 175 -> 113 Ir per element-op (-35%). `ab.sh write` best of 5, copy / zero: store array
10 rows 713 -> 625 ns (+14%), 1k rows 67.9 -> 60.5 µs (+12%), 1k unserialized keys +4%; int, string, object within noise.
Next in the cachegrind profile: `hash_array` (~200 Ir/op, ~60 per 3-element row). Rejected: an `if` chain instead of the `switch` in `hash_zval` (perf put 15% of its samples on the dispatch): 199 -> 207 Ir/op, array stores -2 to -5% (`ab.sh write`, best of 7).

**Scalars and a string alone skip planning** (`put_flat`: no plan, tape buffers or release; cachegrind had 40 of a store's 652 Ir
zeroing the plan). `./dev cg`: put int 652 -> 583 Ir, string 100 B 700 -> 629, `put_multiple` of 10 5632 -> 4966. `./dev ab HEAD 7 write`,
copy / zero: store int +11.3 / +12.2% (25.6 -> 23.0 ns), string 100B +14.3 / +13.1%, failing add +14.1 / +14.5%, store+delete int +5.4 / +4.1%.
**`nc_same_array` compares strings up to 16 bytes inline** (`nocopy_key_eq`, was a `memcmp` call per key and string): `./dev cg` 1k rows
778.3k -> 750.0k Ir; `ONLY='store (array|object)' ./dev ab HEAD 9 write`, copy / zero: 10 rows +15.9 / +15.7% (628 -> 542 ns), 1k rows
+7.2 / +6.7%, 1k unserialized keys +4.1 / +7.9%, object within noise. Rejected: `map_mask` from `__builtin_clz` instead of its loop
(one iteration for small maps): put array 10 rows 8924 -> 8995 Ir, 1k rows 778.3k -> 779.8k.

**Array stores are benched both ways**: the old rows re-store one value, so every row is already shared and nothing is written.
`store array 10 / 1k rows, new values` (`bench/run.php`) and `put_new` / `put_newbig` (`bench/prof.php`) give every row a new `id`
first, the store after a miss (their figures include that loop). `./dev cg`, re-store -> new: 10 rows 8786 -> 15108 Ir, 1k rows
750k -> 1286k (`write_zval` 352k, `hash_array` 228k, `plan_array` 204k, `plan_zval` 175k); `ab.sh write` 10 rows 544 vs 1368 ns,
1k rows 56.6 vs 131.5 µs; HEAD against the two changes above on the new rows: -4.2 / +0.4% and +0.6 / +5.4% (noise: neither is on that path).
Which of the two real stores resemble (`NOCOPY_CFLAGS=-DNOCOPY_SHARE_STATS ROWS_ONLY=1 CONFIGS=stock ./dev drupal-bench`, counters in
`nocopy_plan.c`, a fresh arena filled row by row): of 6,402 array and object stores, 3,443 (54%) found at least 75% of their hashed bytes
shared, 835 50-75%, 1,730 25-50%, 174 under 25%, 194 (3%) nothing; 20.5 of 39.6 MB hashed and 71,023 of 109,752 arrays were not planned
or written, 35,647 of 65,245 lookups hit; all 6,487 puts 47.4 ms. So hashing first pays on nearly every store. Not built for that reason:
hashing only every Nth store while nothing is shared (would fire on 3%), hashing inside the planning walk with a rollback on a hit
(estimated -12% on new values, +30% on shared ones), sharing at merge time instead of at the store (every store would write the half it skips now).

**Cost of every API call** (`./dev cg` instructions per call, `./dev perf` ns, copy mode, PHP 8.5 arm64, `bench/prof.php`; the loop itself is ~41 Ir, ~3 ns):
get int 270 Ir / 12.4 ns (`nocopy_fetch` 162), miss 207 / 9.3, ttl 304 / 16.2, 100 B string 471 / 22.6, by-ref `$hit` 394 / 17.2, `get_multiple` of 10 2957 / 127;
put int 652 Ir / 31.8 ns (`put` 403), ttl 698 / 34.3, add (fails) 626 / 33.9, put+delete 967 / 39.2, `put_multiple` of 10 5644 / 270; `inc` 291 / 12.5, with ttl 366 / 21.9,
`cas` 370 / 16.8, `delete` miss 218 / 9.7, `delete_multiple` of 10 misses 1152 / 47.6, `remember` hit 383 / 21.5, miss 2447 / 160, `shared_with` 272 / 11.2;
`stats` 15570 -> 14777 Ir (array pre-sized: 824 ns before), `keys('m')` over 100k entries 8.9M Ir (89 per entry walked). Nothing but arrays and objects
(see Writes) stands out: every scalar call is within ~3 ns of PHP's own call overhead, so no change was made to them.

**Short value strings are deduped and map hashes sized to their elements**, for the arena's size: Drupal's cache values took
3.3x igbinary's bytes once objects became graph nodes (bench arena 11.1 -> 19.7 MB). Every row of Drupal's database bins after
a warmed `./dev drupal-bench` (786 values), put one by one: serialize 6.26 MB, igbinary 3.80 MB, nocopy 12.62 -> 10.77 MB (-15%).
Of the 12.62 MB, strings were 4.75 (3.59 unique), buckets ~2.80, hashes ~1.64, array headers ~1.37 (24.5k arrays), object nodes
~0.68: 18k of 20k maps hold at most 8 elements (8.2k hold one) and each had PHP's minimum 16-slot hash (64 B). Simulated before
building: equal strings up to 64 bytes in the existing 256-slot key table save 1.06 of 1.23 MB possible (exact table, any length);
a 1024-slot table 1.17, up to 16 bytes 0.74. Hash slots at 2 per bucket (PHP's own ratio, opcache's after compaction) save ~0.8 MB.
`ab.sh write`, copy / zero: store array 10 rows 517 -> 604 ns, 1k rows 50.7 -> 58.5 µs, 1k unserialized keys 56.1 -> 66.0 µs
(+13-17%, three quarters of it the rechaining; one pass copying and chaining each bucket won back 2-4%); other stores and all
fetches within noise. `bench/run.php` 1k rows 520,096 -> 425,472 bytes. A write is paid once per value, the bytes for as long
as it stays cached.

**Strings shared across entries (atoms)**: strings up to 64 bytes are stored once for the whole arena (OPcache stores interned strings once for all scripts; nocopy deduped per value only). Every row of a warmed Drupal 11's database cache bins, put one by one (`ROWS_ONLY=1 CONFIGS=stock ./dev drupal-bench`: front, RSS, JSON:API, 300 nodes, 50 terms; 6,477 rows): 46.16 -> 38.37 MB plus 0.29 MB of atoms, -16% (the estimate, every string stored once with no first copy, was -20%); after the plain warm-up only (785 rows) 11.08 -> 9.72 + 0.24 MB, -10%. `bench/run.php` 1k rows 425,472 -> 352,320 bytes. `ab.sh write`, best of 7, copy / zero: store array 10 rows -4 / -5%, 1k rows -6 / -5%, object -2 / -3%, string 100B -2 / -2%, int within noise; all fetches within noise (hits stay zero-copy: entries point at the atoms).
Hit ratio at equal `shm_size` (`ROWS_ONLY=1 CONFIGS=stock REPLAY="16M 24M 32M"`, `bench/drupal/replay.php`: those 6,477 rows read through, HEAD vs this; the half's reserve is part of it): Zipf 0.99 16M 79.7 -> 81.8%, 24M 86.3 -> 88.8%, 32M 91.9 -> 94.6% (a third fewer misses); rows in a loop 24M 2.0 -> 18.3%, 32M 40.1 -> 57.5%; failed puts at 16M 164 -> 40.
Admitted on the second sighting (a doorkeeper of tags), so a string met once never takes room; the write that admits it already uses it. The area was two halves of `shm_size / 128` (0.29 MB covers the 46 MB above; `/ 32` since arrays share it), each one generation; a full half's successor takes the other half only once no segment's entries and no writer in progress use it ([architecture](architecture.md#atoms-strings-shared-across-entries)).
Built along the way, measured and dropped: an area that is never reset (fills with strings nobody uses and stays full for the arena's life); freeing it in `nocopy_clear`, as first planned (zero-copy readers keep atom pointers past a clear, as do writers between sizing and commit: test 086 reads overwritten strings without the reference check); a probe of the shared table per string occurrence (store array 10 rows -37%; the value's own table first: -16%); persisting probing again and agreeing with sizing through a published-bytes watermark (sizing now records its answers in walk order and persisting replays them: -10 -> -5%, and the admitting write can use its new atom: 785 rows 9.88 -> 9.72 MB); the record list's inline buffer in the zero-initialized `sizing` (store int -20%). Not built: class names as atoms, 152 KB per entry vs 60 KB once over the 785 rows (under 1% of the arena; persisting walks classes in another order than sizing, so it needs its own mechanism).

**Arrays shared across entries** (hash-consing: an array equal to one already stored is stored once for the whole arena, as an atom).
Measured first with a model (`bench/drupal/share.php`, the 6,477 rows above): sharing every repeated array would save 34-42% of the
entries' bytes arena-wide, 5-6% within one entry only; 4,403 distinct arrays (3.1 MB) carry all of it, the top 40 (page chrome,
`#cache` metadata, response headers, `x-default` field wrappers) 13.3 of 20.4 MB, and 98% sits in arrays of 128 bytes or more.
Built: the same rows 38.38 -> 26.02 MB plus 1.47 MB of atoms (0.29 before), -29%; the model under the built rules (two private copies before
sharing, strings over 256 bytes unhashed) said -29 to -37%. Every row reads back identical. Read-through replay of the rows
(`REPLAY`, HEAD -> this), hit % and evictions: 16M zipf 82.5 -> 85.6 (21,378 -> 17,219 evicted), loop 0.2 -> 11.0;
24M zipf 89.5 -> 94.6 (12,213 -> 4,941), loop 18.6 -> 52.7; 32M zipf 94.7 -> 98.2 (4,790 -> 0), loop 53.9 -> 100.0; puts too big for a 16M
arena 43/272 -> 0/0.
Price, `ab.sh write` best of 5 vs HEAD, copy / zero: a value whose every row is shared (the bench re-stores one array): store array 10 rows
-11 / -11%, 1k rows -4 / -6%, 1k unserialized keys +1 / +2%; object -4 / -5%, int and string 100B -2 to -4%. Fetches unchanged (they get an
ordinary immutable array). Equality is always checked: the hash only finds a candidate. Trusting a 128-bit hash instead would drop the
compare, but string hashes are PHP's DJBX33A, which collides by construction, so equal hashes over different arrays are easy to make; a
keyed universal hash over every string's bytes would be needed and was not built.
The first version decided bottom-up inside the planning walk: plan an array, look it up, compare, roll its plan back on a hit (strings first
met inside it marked for re-planning), one decision per source array in a pointer map. A fully shared value then paid three walks to write
nothing (perf, 1k rows: planning 32%, the map and lookups 26%, compare 32%): 10 rows -33%, 1k rows -34%, unserialized keys -43%, and only
-25% on the rows (outer arrays never matched once their inner ones were already shared). Hashing first and planning top-down skips hit
subtrees whole; decisions on the tape instead of the map drop the map (its allocation, zeroing and a probe per array written); the sharing
state in its own struct, set up by array and object writes only, kept scalar stores from zeroing it (int -7 -> -2%); no region for a flat
array of scalars under 128 bytes (object -14 -> -4%). Also measured and dropped: the hash through a pointer to stack temporaries (object
-11%), two dependent multiplies per element, admitting up to 1,024 arrays per write instead of 64 (no change), the doorkeeper off (~5 points
more saved, but the area fills with arrays met once), the area at `/ 128` vs `/ 32` (`ab.sh write` within ±2%).

**Planning decides, writing replays (the tape).** Superseded 2026-11-12 by one walk into a private buffer (below). Sizing and persisting used to agree because both walks took the same decisions
(the string table probed twice, the object and reference maps probed again while persisting; only atoms were recorded and replayed).
Now planning records every decision in walk order (a deduped string's index and whether it comes first, an object's index, a shared
reference's index or "held once") and writing looks nothing up: it cannot disagree with the size, and it allocates nothing (class
lowercase names are made while planning; before, `zend_string_tolower` ran under the shard lock for graph blocks up to 2 KB).
`./dev ab main 7`, copy / zero: store array 10 rows -1.9 / -2.2%, 1k rows -2.7 / -2.0%, 1k rows unserialized keys -2.3 / -3.2% (a tape
entry per string); store object +10.8 / +8.7% and fetch object (zero) +5.5% (no map probes while writing); other stores and fetches within ±3%.

**Planning decides, writing replays and checks (tagged ops).** Superseded 2026-11-12 by one walk into a private buffer (below). Every decision the writer used to re-derive from the value (long string,
empty array, reference held once, object first or again, array while sharing is off) is now a tagged op on the tape; the writer takes one
op per non-scalar slot, checks it against the slot and the bytes left, and a failure (release builds too) aborts the put: store refused with
a warning, never a corrupt or overrunning block. Rebase descends only through pointers that moved; `block_check` also runs after rebase in
checked builds. Final `./dev ab HEAD 7 write`, copy / zero: array stores -0.8 to -2.6%, object -5.3 / -5.1%, scalar and lone-string stores
+6 to +10% (no writer struct for them), fetches within noise, block bytes unchanged (240,544). Rejected: a writer driven by indexed ops
without a walk (arrays -2.6 to -5.9%, and it cannot notice a slot it was never told about); relocation data in blocks (bitmap +1.56%
bytes per array block, u32 list +2.5 to 6%; would move array blocks 1.8 to 3x faster, left for later); hashing every string on write
(string alone 100 B 31-37 -> 60 ns, 10 KB 222 -> 2,690 ns).

**One walk into a private buffer, placed by a copy and a relocation list** (2026-11-12; were: planning sized the value and recorded a
tape, writing replayed it into the block under the lock). One walk (`nocopy_build.c`) copies the value into a per-thread buffer, every
pointer as a block offset with its place listed; placing is a `memcpy` and one add per listed place. Gone: the sizing pass, the tape, the
replay checks and their `longjmp`, the "value changed between planning and writing" refusal (test 103), multi-pass hook planning.
Hooks run as met, in `serialize()`'s order and with its semantics (a hook that changes a part already walked does not change what is
stored; test 074 now compares with `serialize()`); the walk holds what it still reads before user code runs. A reference found shared
after it was built as its value restarts the walk once, knowing it. Value layer 2,224 -> 2,132 lines.
Rebasing the copied block with `nocopy_value_rebase` instead of a relocation list cost full writes 11-12% (`ab.sh write`, 1k new rows);
the list brought it to -6%, and cachegrind showed the builder already running fewer instructions than plan + write: the rest was the
block's `memcpy` (perf: `__memcpy_generic` 1.1 -> 7.5% of `put_newbig`) and the relocation loop. Tuned by leaving scalars as the table copy
brought them (no call, no write-back), the buffer pointers in the builder instead of behind `env`, a one-mask test for pointer slots.
Final, cachegrind Ir per call HEAD -> this: put_arr 8,951 -> 8,506, put_new 15,363 -> 13,851, put_big 762,899 -> 724,872, put_newbig
1,310,458 -> 1,159,087, put_obj 4,850 -> 4,059, put_prods 272,906 -> 248,745, put_ords 170,204 -> 151,222, put_json 326,046 -> 304,026;
get_prods, get_ords, get_json unchanged. `./dev ab HEAD 9 write`, copy / zero: store object +12.0 / +11.5%, array 10 rows +5.1 / +5.7%,
1k rows +4.4 / +3.2%, 1k unserialized keys +3.0 / +5.0%, 1k new rows +1.3 / +2.3%, 10 new rows -1.5 / -1.8%, int, string, store+delete
within ±2%. `./dev ab HEAD 7 read`: fetches within ±3%, block bytes unchanged (240,544). The lock is held for the copy and the adds only.

**`nocopy_delete_multiple`**, because Drupal's `deleteMultiple()` and Symfony's `deleteItems()` deleted key by key, one `UNLINK`
round trip each with `nocopy.resp`. `bench/delete.php`, RESP on loopback (medians of 21): 10 keys 340 -> 39 µs, 100 keys
3.3 ms -> 75 µs, 1000 keys 33 -> 0.44 ms. Locally 100 keys 2.8 -> 1.9 µs, 1000 keys 33 -> 22 µs. The APCu polyfill keeps its loop:
`apcu_delete()` returns the keys that failed, and a count cannot say which.

**Deletes use the key's cached hash** (`zend_string_hash_val`; was `zend_hash_func` over the bytes on every delete). Scratch A/B,
best of 7: store+delete with a 58-byte key 85.3 -> 68.9 ns, `nocopy_delete_multiple` of 10 missing 59-byte keys 177.5 -> 50.7 ns;
`ab.sh` store+delete int (3-byte key), best of 7 three times: +1.6..+3.4% (HEAD against itself +1.0 / 0.0%).

**Monotonic clock for TTLs, leases and `seq`** (2026-11-10, `CLOCK_MONOTONIC_COARSE`):
the arena does not outlive a boot, so a stepped wall clock (NTP, an admin, a VM resume) neither expires entries early nor
keeps them late. Read cost in the dev image, 2x10^7 reads, 3 runs: `MONOTONIC_COARSE` 3.6-4.1 ns, `REALTIME_COARSE` 4.0-4.2,
`BOOTTIME`/`MONOTONIC`/`REALTIME` 11.3-13.7. `CLOCK_BOOTTIME` was the plan's first choice (it counts suspend) and is
rejected: it has no coarse variant, 3x per read, and servers do not suspend; a suspended laptop keeps entries that long
longer. Wall time stays where time leaves the arena: `start_time`, `nocopy_keys()` (converted, within a second) and the
RESP payload. RESP gets `SET … PX <ms>` (relative, was `PXAT`): RESP's clock is never compared with ours. The
payload's absolute second only bounds a reader's local copy; it no longer turns a value RESP still holds into a miss.
Test 100 (a writer an hour behind or ahead, libfaketime) and 101 (the same against RESP) fail on the wall clock.
Processes in different time namespaces (`unshare --time`, CRIU) must not share an arena.

**Integers read from RESP take their expiry from `PTTL`.** They are stored bare (decimal, so `INCRBY` works), with no
header and so no expiry second; their local copy never expired and outlived the key until an invalidation for RESP's
expiry arrived (Drupal's `testFlood` passed 1 of 6 runs). A read that got integers pipelines one `PTTL` each (relative,
like `PX`) and keeps the copy to the last whole second the key still lives, or keeps none. Costs one round trip more on
an integer's local miss; reads without integers send nothing more. Rejected: `PTTL` with every `MGET` (a command per key
on every miss) and a header on integers (`INCRBY` needs the bare number). Test 105.

**Coarse clock** (`CLOCK_REALTIME_COARSE` where it exists, before the monotonic one above): TTLs are seconds, the coarse clock is a tick (1-4 ms) behind at most and costs 3.5 ns
vs `CLOCK_REALTIME` 11.2 ns (perf: half of a store's samples were in `clock_gettime`). `ab.sh` best of 5, copy / zero: store int +9 / +9%,
store string +9 / +9%, failing add +6 / +4%. `seq` carries the shard in its low 3 bits, so it is unique across shards without the nanoseconds.
Scalars skip `copy_out` and the pin: fetch int +7 / +4%, `fetch 10 keys` (ints) copy +11%.

## Eviction

**Segcache-style merge** instead of APCu's full wipe: the oldest segments are merged, read entries move forward, the rest go.

**No per-entry hit counters:** an exact counter is a shared write on every read. A Morris-style probabilistic counter
(0-15) writes a cache line about log2(reads) times between merges.

**Merge victims: the oldest segment plus the 3 least read of the 32 oldest** (after GL-Cache's group-level eviction), ranked by a
per-segment sum of the entries' read estimates kept up to date on every `hot` bump. `bench/hitratio.php`, 4M / 32M vs the 4 oldest:
Zipf 91.2 -> 92.2 / 92.9 -> 93.9, Zipf + 0.5 one-hit wonders 80.1 -> 88.0 / 83.2 -> 89.4, loop 1.1x 25 -> 49 / 25 -> 50, loop 1.5x 25 -> 25 / 18 -> 25;
20% overwrites 84.9 -> 84.0 / 87.3 -> 87.2. Puts under eviction and fetches unchanged. Always taking the oldest keeps scans from sticking.

**Segments stop growing at 4 MB; bigger arenas get more of them** (up to 8,192 at 32G) instead of 256 ever larger ones.
An eviction round holds the shard lock while it moves up to a segment's worth, so writers waited in proportion to the
segment size. `./dev big` at 4G (16 MB -> 4 MB segments), 20 s of evicting writes: put max 51-111 -> 26-42 ms, puts over
50 ms 0-7 -> 0, over 10 ms 1,860 -> 646-658; rounds 4x as often, so puts over 1 ms 2,300-2,700 -> 7,800 and p99.99
0.26-0.34 -> 4.8 ms (what 1G already had); gets 3.4-3.7 -> 4.2 M/s. Bitmaps and the segment table moved behind the slot
tables, sized by the segment count, so arenas up to 1G keep their layout (`./dev ab`: within HEAD-vs-HEAD noise) and 1,000
idle slot holders at 4G still cost nothing (put p99.99 441 vs 422-502 us).

**Compact a segment alone up to 3/4 live**, not only segments at <= 1/4 that fit together into one: rewrites spread
their dead copies evenly, and at 20% of hits rewritten only 63% of the taken segments were live, so merge evicted while a
third of the arena was dead. `bench/hitratio.php` 32M / 64M, 20% rewrites 87.4 -> 91.0 / 85.3 -> 90.5, 50% rewrites
84.3 -> 90.2 / 83.6 -> 89.8; workloads without rewrites unchanged. It copies more (20% rewrites: 184 -> 808 MB moved):
`ab.sh` ttl churn stores 2.37 -> 2.17 M/s, stores > 50 us 1.3k -> 3.6k, the slowest unchanged; other writes and fetches unchanged.
Up to 1/2 live: +1 / +4 points; up to 7/8: +1 more point than 3/4 for twice the copying.

**A segment is compacted alone only for its dead bytes** (`3 * dead >= live`; was: at most 3/4 live, which counted its unused tail as
reclaimable). In a full segment both say the same (3/4 live, 1/4 dead); a segment of two values of a third of a segment each is 2/3 live
with nothing dead, so compacting it filled a new segment the same way, counted as progress, and the alloc loop copied such segments in
circles and never evicted: stores failed for good. 600 puts of distinct keys with values of `segment_size / 3`, 4M / 32M: failed 494 / 158 -> 0,
evicted 0 -> 496 / 160, moved 2,341 / 1,550 MB -> 0, 148 / 120 -> 1 / 6 us per put (test 107; found by `tests/c/sched.c`'s setup).
`bench/hitratio.php` 32M / 64M, old -> new: zipf 93.8 -> 93.8 / 93.3 -> 93.2, + 0.5 one-hit wonders 89.6 -> 89.6 / 91.1 -> 91.1,
20% rewrites 90.6 -> 90.6 / 90.1 -> 90.1, 50% rewrites 89.9 -> 89.9 / 89.4 -> 89.4 (moved 1,921 -> 1,879 / 3,494 -> 3,437 MB),
loop 1.5x 25.1 -> 25.1 / 40.2 -> 40.2, loop 1.1x at 64M 69.3 -> 69.2 and at 32M 50.1-52.9 -> 50.5-52.3 over four runs each,
6 x 3 MB 83.5 -> 83.6 / 91.1 -> 91.2, with 20% rewrites 77.6-77.8 -> 77.5-77.8 / 87.7 -> 87.7. Not measured: `./dev ab` (one comparison per merge).

**A merge takes its victims one at a time, and a compaction that runs out of segments stops instead of evicting.** It moved the
survivors of all (up to 4) victims, then unlinked what was left in them; a move that found no segment ended the moving, so the rest of a
"lossless" compaction was evicted. A victim's survivors go to its own TTL bucket's open segment: victims of two buckets, or one whose
bucket's open segment is full twice, need more than the one segment kept free for merges. Now each victim is finished (moved, its
expired entries unlinked) before the next, and a move without a segment first has `reclaim()` give the finished ones back; a compaction
that still finds none keeps the rest, and only one that moved nothing evicts its sparsest victim.
Arena alone, 4M (54 segments of 64 KB), values of 1-5 KB with TTLs over all 7 buckets, 1M mixed operations (45% puts, deletes, reads, a
clock that expires the short TTLs), 300 / 500 / 700 / 900 keys = 19 / 29 / 43 / 53% of the segments live: evicted 0 / 637 / 6,971 / 14,511
-> 0 / 0 / 0 / 0, every one of them by a compaction (no eviction round ran), moved 0 / 68 / 206 / 426 -> 0 / 70 / 247 / 555 MB;
1,100 keys (64% live): 0 evicted, 1,170 MB moved. `tests/c/merge.c` (500 keys, 300,000 overwrites, nothing expires): 5,992 evicted in
1,045 rounds -> 0. `bench/hitratio.php` (one bucket, more keys than room) 32M / 64M, old -> new: zipf 93.8 -> 93.8 / 93.3 -> 93.4,
+ 0.5 one-hit wonders 89.6 -> 89.6 / 91.0 -> 91.0, footprint 3 86.2 -> 86.4 / 85.5 -> 85.4, 20% rewrites 90.6 -> 90.5 / 90.1 -> 90.1,
50% rewrites 89.8 -> 89.9 / 89.4 -> 89.4 (moved 1,878 -> 1,884 / 3,463 -> 3,466 MB), loop 1.5x 25.1 -> 25.2 / 40.2 -> 40.3; loop 1.1x
52.0 -> 50.1 / 69.3 -> 68.6 in that run, then four more runs of each build with the same evicted and moved counts run for run.
`./dev ab HEAD 3 write` (no row merges under pressure; another benchmark ran beside it): -2.1..+4.7% copy, -10.3..+6.3% zero-copy, signs mixed.
Not measured: a PHP workload with mixed TTLs, pauses under the shard lock (`./dev tail`).

**A segment holding one entry over half a segment is not compacted.** Such an entry gets a segment of its own; compacting it
needed another whole segment, freed nothing and counted as progress, so the alloc loop copied it up to 2x the shard's share per store
and never evicted: stores failed instead and readers kept the old value. `amp` scratch script (random overwrites, half reads),
default 32 MB, values 1-120 KB: footprint 0.8 64.8 -> 6.1 µs/op, moved 52.7x -> 1.9x the bytes written, failed stores 1,693 -> 38
(hit 89.1 -> 83.1%: the old figure counted stale values); footprint 1.2 135 -> 7.7 µs/op, 119x -> 3.5x. Values up to 20 KB and 500 B
unchanged (test 077).

**Entry header 40 bytes, ghost tags 16 bits** (were 48 and 32): the header holds its size / 8 and its expiry in 32 bits
(monotonic seconds, as `created`; an expiry past them is capped at 136 years of uptime). Per entry, 8 bytes: int 72 -> 64,
8-byte string 112 -> 104, 100-byte string 207 -> 199, 3-key array 256 -> 248. The fixed header (slot tables, ghosts, atoms,
workers) of a 32M arena 2,560 -> 2,304 KB (7.8 -> 7.0%, 236 -> 238 segments), 128M 9,216 -> 8,192 KB (7.0 -> 6.2%).
`./dev ab HEAD` 5-9 reps, read and write: within noise but store string 100B, 2-7% slower in three runs (28-29 -> 30 ns;
store int 1-2% faster, so not the put path's code: where that block's bytes land, not looked into).
Looked at and left: the slot tables (a slot per 256 bytes, 3.1%: fewer evict small entries by count sooner), the atoms'
reserve (2.1%, measured above), the 1,024 workers' lines and bitmaps (168 KB at any size: 4% of a 4M arena, 0.5% of 32M).

**Keys stored as bytes, not `zend_string`s:** a block's key is only compared, so its 24-byte header and NUL went
(500-byte value: 624 -> 600 bytes). 32M / 64M: Zipf 94.1 -> 94.6 / 93.6 -> 94.1, loop 1.1x 57 -> 76 / 72 -> 76,
loop 1.5x 25 -> 35 / 41 -> 49 (on top of the compaction change).

**Ghost list (S3-FIFO):** one 16-bit tag per slot (32-bit until 2026-11-10: a false match, 1 in 32,768 per occupied cell, only starts a new key hot). Measured with `bench/hitratio.php`: no ghost loses 2 points on mixed
loads and all hits on scans.

**No `nocopy.ttl` default:** it expired Drupal's cache tag tokens. Callers pass `$ttl`.

**Drupal `get()` has a path of its own, and tags are preloaded only with `nocopy.resp`:** `get()` went through
`getMultiple()` (key maps, `array_diff`, a tag preload whose tags `isValid()` asks for right away), and the checksum trait's
`isValid()` copies and merges arrays even when every tag is loaded. Now: one `nocopy_get_multiple` of version key and item
(still one round trip on a RESP miss; two `nocopy_get` would be two), prefix and version key built once, the item object
cast from an array, `isValid()` in `NocopyCacheTagsChecksum` summing the loaded tokens itself and falling back to the trait
when a tag is missing, delayed or waiting for preload. `getMultiple()` preloads tags only with RESP, where one lookup saves
round trips; locally it cost more than the lookups. `./dev drupal-get`, ns per call, best of 5 x 20k, CLI (copy mode),
bare `nocopy_get` 34: miss 499 -> 127, hit without tags 935 -> 335, 3 tags 1,572 -> 533, 20 tags 3,447 -> 1,170,
3 tags expiring 1,684 -> 600, `getMultiple` of 10 per item 929 -> 510, of 10 misses 130 -> 99. A miss no longer creates a
missing bin version (the next `set()` does). What is left of a hit: the copy of the item (zero-copy under fpm), the object,
and `explode()` of the tags (stored as one string). Not measured per page.

**Drupal bin version in the item, not the key:** a versioned key orphaned every item on `deleteAll()`, and with
`nocopy.resp` the TTL-less small bins (config, discovery, ...) then stayed in RESP for good after each `drush cr`.
Not Redis's `last_delete_all` timestamp: that compares clocks of different servers. `invalidateAll()` deletes (as Redis
does by default), so each call reads one bin key instead of two.

## Memory balance

**Shards take segments from a shared pool** (was: a fixed slice each, nothing lent). A shard below 15/16 of its share whose
pool is dry has the shard holding the most evict once for it; above its share it merges its own. `bench/hitratio.php zipf <fp> 0 0 <big> <mb>`
(hot big values land on few shards), 32M / 64M: 6 x 3 MB 90.8 -> 95.7 / 95.6 -> 97.8, 3 x 5 MB 92.2 -> 97.6 / 93.5 -> 98.1,
10 x 2 MB at fp 0.8 92.2 -> 97.9 / 97.7 -> 99.5, 6 x 3 MB at fp 1.5 86.7 -> 88.7 / 91.2 -> 92.1. Without big values (4M / 32M / 64M):
zipf 92.2 -> 92.2 / 93.9 -> 94.1 / 93.1 -> 93.7, loop 1.1x 49.9 -> 49.8 / 50.0 -> 60.8 / 67.6 -> 72.5, 20% overwrites 84.0 -> 84.1 / 87.1 -> 87.4 / 85.1 -> 85.4;
zipf + 0.5 one-hit wonders 88.0 -> 88.0 / 89.4 -> 89.5 / 91.6 -> 91.3 (the price: shards drift around their share).
The 15/16 slack: rebalancing at any deficit cost the one-hit-wonder load 0.6 points at 32M and 64M; 7/8 lost most of the
6 x 3 MB fp 1.5 gain (88.6 -> 87.1 at 32M), 3/4 cost 64M 1.1 points; 15/16 was best or tied in all four.
`take_run` scans from the shard's home range: first-fit from segment 0 interleaved the shards, whose `nocopy_seg`s
(56 B) then shared cache lines: parallel stores x4-x16 -44 to -60%. The fast path of `nocopy_seg_alloc` is split from
taking a segment (its registers lengthened the prologue: +5 instructions a write); a write now runs 9 fewer instructions
than before (cachegrind), `ab.sh` within noise. Price: a write that needs a segment scans `owner[]` (256 bytes) and may
drop its lock to rebalance; an entry is still limited to a shard's share.

## Single flight

**Leases in a fixed per-shard table, not in the cache.** As cache entries they used slots, showed up in counts, were wiped by
`clear`, and a drop could hit a retaken lease.

**Lease waiters get the expired value** (stale-while-revalidate, as memcache's leases at Facebook) if it expired less than one
TTL ago. XFetch already avoids most expiries under steady traffic; this covers bursts after idle. `bench/stampede.php`, workers
waking together after a gap longer than the TTL: requests slower than half the compute time 112 -> 22 (16 workers, 100 ms),
83 -> 24 (300 ms), 158 -> 37 (32 workers); callback runs unchanged. Bounded to one TTL so an idle shard (no sweeps) never serves
arbitrarily old data.

**The callback is resolved only when it runs.** `Z_PARAM_FUNC` checked and resolved it on every call (perf: ~25% of a hit in
`zend_is_callable_ex` and `zend_fcall_info_init`); a Closure now only gets a type check, other callables `zend_is_callable_ex`
(still a TypeError on a hit). `ab.sh` best of 5, `apcu_entry hit` (`nocopy_remember`), copy / zero: 24.7 -> 18.7 / 24.1 -> 18.6 ns.

**A user callback gets `&$ttl`** (internal ones only the key: they reject extra arguments); `$ttl < 0` stores nothing. Symfony's
`get()` callbacks pick their lifetime while they run (`$item->expiresAfter()`) and may veto the save (`$save = false`), so this
is what lets the bundle route `get()` through `nocopy_remember`.

## Symfony

**The bundle's `get()` is `nocopy_remember`**, not Symfony's: `AbstractAdapter::get()` runs `getItem()` + metadata, wraps every
value with an expiry in a `ValueWrapper` object (so nocopy stored it serialized, not zero-copy), and locks misses with
`flock` per host under php-fpm. `bench/sfget.php` (CLI, so without Symfony's lock; 8.5, Symfony 8, a 20-int array + 100-byte
string), two runs: hit with `expiresAfter` 1,958 / 2,097 -> 190 / 217 ns, hit without expiry 682 / 655 -> 197 / 207 ns,
miss + store 3,534 / 3,415 -> 1,559 / 1,481 ns. Not carried over: `$metadata` (left empty; ChainAdapter takes it from the pools
below), `$item->isHit()` on an early refresh (always false), `setCallbackWrapper()`, a pending `saveDeferred()` of the same key.

**The bundle puts nocopy in front of Symfony's system pools** (`framework.cache.system`, prepended, so an app's own setting
wins): `AbstractAdapter::createSystemCache()` chains APCu before PHP files only when APCu is loaded, so a nocopy app ran its
validator, serializer and property-info metadata from PHP files alone. `NocopyAdapter::createSystemCache()` builds the same
chain with nocopy (versioned by `container.build_id`, so a deploy reads no old metadata from the arena that outlived it), and
Symfony's own where the process does not share nocopy. `./dev sfbench`, `validate`, req/s, APCu / nocopy: before 9,286 / 8,182
(default pool) and 10,252 / 8,187 (raw pool), after 9,100 / 9,047 and 9,347 / 9,499.

## APCu

**Compatibility as a PHP polyfill**, not in the extension. The extension keeps only `apc.enabled`/`apc.enable_cli`
(Symfony's `ApcuAdapter::isSupported()` and Composer check them). `bench/run.php` measures `nocopy_*` directly by
re-running itself with `apcu_*` rewritten (fetch int 11.7 ns zero-copy, without the polyfill's extra call).

**Plain objects are stored as their property slots** (`NOCOPY_FMT_GRAPH`), not serialized: a fetch resolves each class once,
checks a fingerprint of its slot layout (names, visibility, readonly, types), creates the objects without a constructor and
copies the slots in. Strings and arrays in them stay zero-copy, and so do object-free arrays next to them (one object no longer
serializes the whole value). `bench/objects.php` (8.5, `Product` objects with 6 properties and a shared `Category`), zero-copy /
copy mode: one object 881 / 911 ns unserialized -> 112 / 249 ns, 100 objects 82.8 / 88.0 µs -> 4.6 / 16.2 µs, 100 arrays +
1 object 27.5 / 27.2 µs -> 91 ns / 9.0 µs. `ab.sh` (best of 3-7): fetch object 600 -> 77 ns zero, 620 -> 134 ns copy; store
object 243 -> 182 ns; other stores 0-3% slower (int, string, failing add: within noise of runs; 1k-row arrays ~2%), fetches unchanged.
symfony/php-ext-deepclone's payload stored zero-copy (`deepclone_from_array(nocopy_get())`, 0.8.9) was 473 ns / 10.2 µs for
the first two: it walks a portable array format and checks every property by name, which a cache on one host need not do.
Also nodes: classes with `__serialize`/`__unserialize` (user and internal: `DateTime*`, `ArrayObject`,
`SplObjectStorage`; `props[0]` is the array, `__unserialize` called on fetch), user classes with `__wakeup` (slots, then the call)
and enums (the case). `bench/objects.php`, 100 `Order` objects with a `DateTimeImmutable` and an enum: 85.9 / 83.8 µs ->
48.9 / 47.1 µs (zero / copy); `DateTime::__unserialize` parsing its string is most of the rest. Like arrays, nodes don't
dedup equal strings (igbinary does; since 2026-11-06 short ones are, see [Writes](#writes)). `stdClass` and dynamic properties too: the properties table is stored as an array, and
each fetch gives the object a private copy of it (one bucket memcpy; its strings and arrays stay zero-copy), not the arena's
immutable table: the engine separates such tables on its own writes, but its `(object)` cast still duplicates them, and an
extension writing a table in place would corrupt the cache. `bench/objects.php`, `json_decode` of 100 rows (200 objects):
42.2 / 41.8 -> 8.2 / 9.5 µs (zero / copy). `ab.sh` store object (plain slots) 180 -> 190 ns for the kind checks (243 ns serialized). Other objects (`Serializable`, internal classes without `__serialize`,
lazy, closures) serialize the whole value, as in APCu, with no `$serialize` flag on writes: with the flag, every caller
in the repo (polyfill, Drupal, Symfony) passed `true`, and without it shared references were silently flattened into copies.
A changed class (other fingerprint) or one that can't be loaded is a miss, not an incomplete object.
**Fingerprints include parents and interfaces.** A slot is not type-checked when a graph is rebuilt (`unserialize()` checks it); the
fingerprints stand for that. With only the class's own name and slots, a `Bar` that stopped extending `Foo` but kept its slots still
matched and landed in a `?Foo` property (test 089). Cached per request, as before: no change in `ab`.

**Shared references are graph nodes too** (were: the whole value serialized): a reference met more than once is `NC_RREF`, its value in
the graph's `refs`, rebuilt as one `zend_reference` per fetch (typed slots holding it become its type sources); one met once is
stored as its value, like `serialize()` (except when that value is an object met elsewhere: `serialize()` writes `R:`, so after `unserialize()` both slots are references; nocopy leaves them as they were). 1k rows + one shared ref, copy (`local_size=0`) / zero: fetch 124 / 113 µs -> 32 µs / 42 ns,
store 125 -> 24 µs; 3 keys, one shared: fetch 125 / 116 -> 77 / 42 ns. Price: graphs without shared refs fetch 1-3% slower
(`prof.php`, best of 16 interleaved: get_prods 3647 -> 3773 ns, get_obj 100 -> 103 ns, get_json, get_ords within noise),
stores within noise; the type sources first sat in the slot loop (get_prods -6%), now a pass of their own.
**Classes with slot holes are graph nodes** (were: the whole value serialized): a class that redeclares an inherited property gets the parent's slot for it, and PHP leaves its own slot UNDEF with no property info. The fingerprint records the hole; the slot persists as UNDEF (or, unnamed by `__sleep`, the class default, UNDEF too). Drupal 11's `FieldConfig` and `ConfigEntityType` have one, so 7 of the 6,477 database cache rows above (`cache_discovery`: `entity_type`, `entity_type_definitions.installed`, `entity_bundle_field_definitions:*`) were serialized; now none is, all read back identical (`bench/drupal/rows.php`). Copy-mode fetch, best of 5: `entity_type` 120 -> 11.5 µs, `entity_bundle_field_definitions:node:article:en` 10.1 -> 0.9 µs; the 7 rows take 60 KB more (38.37 -> 38.43 MB). Test 087.

**`__sleep` classes are graph nodes** (were: the whole value serialized): `__sleep` runs before the last sizing pass, its names are found as `serialize()`
finds them (as given, private to the class, protected) and kept as a slot mask plus the named dynamic properties; the other slots are
`NC_DEFAULT` and get the class default on fetch, as `unserialize()` leaves them. Anything `serialize()` would warn about (not an array,
a missing, duplicate or non-string name) serializes the value, which repeats the warning (and calls `__sleep` a second time).
100 entities with 3 of 4 properties named and `__wakeup`, copy / zero: fetch 32.9 / 33.1 -> 11.2 / 3.9 µs, store 24.9 -> 24.5 µs;
one: fetch 349 / 355 -> 183 / 94 ns, store 314 -> 340 ns (the properties table `serialize()` builds too, the second sizing).
Other graphs within noise (`prof.php`, best of 12).
Copy mode keeps a private **template** of a graph in the kept-copy table on its second sighting (the block's bytes with
its strings and arrays copied out, classes resolved); later fetches build the objects from it, sharing those values by
refcount (COW). `bench/objects.php` copy mode: one object 249 -> 104 ns, 100 objects 16.2 -> 5.3 µs, 100 arrays + 1 object
9.0 µs -> 67 ns; `ab.sh` fetch object copy 131 -> 72 ns, other fetches within ±0.3 ns.
Building a graph copies plain slot values inline (zero and template mode) and calls out only for object references, spines
and copy mode: `bench/objects.php` 100 objects, copy / zero: 4429 / 3619 -> 3647 / 3232 ns. Sizing and persisting find
objects (by handle) and classes in an open-addressing table keyed by integer (igbinary's `hash_si_ptr`: multiply + byte swap,
linear probing, half full at most, 32 slots inline), not `HashTable`s: `prof.php` put_json 13764 -> 12173 ns, put_prods
10592 -> 10236, put_ords 40278 -> 39054, put_obj 263 -> 247; fetches unchanged. Packing the class code into the `objs` entry
instead (one lookup fewer) gave 1-3%.
What sizing learns about each object (class code, kind, dynamic properties, `__serialize()`'s array) is kept in a node array
by object index, so persisting does one lookup per object and no `HashTable`s are built for hooks: `prof.php` best of 11,
put_obj 240 -> 212 ns, put_ords 37.4 -> 36.0 µs, put_prods 9.79 -> 9.97 µs, put_json 11.86 -> 12.15 µs (~1 ns per plain
object; not the free loop, regrowth or node size: 8-byte nodes with a side array made put_json 5.6% slower). **Hooks run between sizing passes, not during one** (superseded 2026-11-12: hooks run as met again, with the walk holding what it still reads, see [Writes](#writes); were: called mid-walk, then a second pass reused their results by walk
index): a `__serialize` that replaced an object already walked gave persisting another graph than sizing (block size assertion,
test 074), and a hook could grow an array the walk was iterating. A pass skips the contents of objects whose hook has not run,
the hooks run, the next pass sizes everything; results are found by object (held), not by index. `prof.php` best of 9
interleaved: put_ords 51.4 -> 48.2 µs, put_prods 14.8 -> 14.1 µs, put_json 18.9 -> 18.1 µs, a `__sleep` value of 100 entities 17.1 -> 16.4 µs
(the first pass no longer sizes hook objects' contents), put_obj 260 -> 254 ns, put_int / put_arr within noise. Results kept
in a map from the start (512 B more in every store's frame) made put_int +3.7% and put_obj +5%: up to 4 are searched
linearly. A `__serialize` result holding more hook objects costs one more pass; hooks run outer first, siblings before the
ones their results hold (`serialize()` runs them depth first). Profile after (`./dev perf get_prods ...`): a fetch of
100 objects is ~36% nocopy, the rest the engine creating and freeing objects; `DateTime*` objects cost ~550 ns each, over 95%
in ext/date's `__serialize`/`__unserialize` (string formatting, parsing, timezone lookup).

**`DateTime` and `DateTimeImmutable` are stored natively** (kind `K_DATE`: timestamp, microseconds, zone type, offset, zone ID or
abbreviation), not as `__serialize()`'s string. timelib's functions are not exported from the php binary, so a fetch uses ext/date's:
`php_date_initialize_from_ts_long` (UTC), then `date_timezone_set()` with a `DateTimeZone` filled from the stored zone (IDs resolved
once per request through `DateTimeZone`'s constructor, so ext/date's tzdb and cache); an immutable date is built in a `DateTime` that
hands over its `timelib_time`. Subclasses stay `__serialize` nodes. `prof.php` best of 7 interleaved, 100 `Ord` objects (a
`DateTimeImmutable` and an enum each), copy / zero: get_ords 43.2 / 43.1 -> 23.2 / 23.3 µs, put_ords 39.7 / 40.1 -> 7.2 / 7.3 µs.
A fetched date still costs ~190 ns (the engine's object allocations and the internal call); unlike `unserialize()`, an ambiguous
local time (DST fall-back) keeps its instant.

**The Drupal module requires the polyfill.** Without APCu, `function_exists('apcu_fetch')` is false, so Composer's class loader
(`setApcuPrefix`) and Drupal's FileCache fall back to the file system: 807 `faccessat` per logged-in request vs 65 with the
polyfill (strace, 2,000 nodes). Median of 3 interleaved runs, nocopy-all: logged-in +21-31%, anonymous +11-39%.

## Build

**The value layer by phase, without module state.** `nocopy_plan.c` (may allocate, runs hooks), `nocopy_write.c` (under the shard lock: `./dev layers`
rejects allocator and user-code calls there), `nocopy_read.c`, `nocopy_class.c`, `nocopy_serial.c`, entry points in `nocopy_value.c`; the store passes a
`nocopy_env` instead of the value units reading `NOCOPY_G`. The value layer still calls `nocopy_put_begin`/`commit` itself: handing that to the store
needs either a sink of callbacks (two indirect calls on a ~27 ns store) or the planning state in the public header. Not a directory like `arena/`:
it needs PHP, so it cannot be built or tested alone. Measured on the way (`./dev ab main 7`, copy / zero): with the root's planning out of line
(a call per store) the failing add was -8.1 / -6.9% in three runs; forcing `encode` inline while that call remained, store int -6.3 / -6.2% and
string -10.2 / -7.3%; the root planned inline (scalars and a lone string in the header) and `encode` inline: store int +2.5 / +1.9%, failing add -1.2 / -4.7%.
A zero-copy scalar fetch through the mode dispatch was -5% (`ab` best of 9: fetch int, `exists`, parallel fetch int x1-x8 -4.5 to -5.7%):
`nocopy_value_scalar` first, as the old read path did, brings them to within ±2% (parallel +0.4 to +2.6%).

**The PHP layers' boundaries checked, not agreed.** Kept copies take their state as a parameter (`nocopy_kept_cache`, the value layer's env) and include `nocopy_value.h` only; the RESP tier
has its own header for the module (`nocopy_resp.h`: state and six calls; its INI lives in `nocopy_resp.c`); `nocopy_keys`' walk is the store's. `./dev layers` enforces each, and that no
`nocopy_value_int.h` name reaches past the value units through `nocopy.c`'s one translation unit (`GROW` did). Each step `./dev ab main 7`, copy / zero: fetch and store rows within ±3%;
reruns of the rows outside (ab main 9-11) flipped sign: fetch string 100B copy -4.8% then +1.2%, parallel arr1k zero x2 -6.3% then +6.5%, 4 readers + 4 writers zero +5.2 / -7.2 / +4.5%
in three runs, and the "4 readers + 1 writer" copy row ranges 74-1415 k ops/s on unchanged code. A ±3% bar on those rows needs more rounds than `ab` takes.

**One translation unit for the PHP layers.** `nocopy.c` includes the value units (phases first, entry points last), `nocopy_kept.c` and `nocopy_store.c`, which still compile alone (tidy).
Splitting objects out of `nocopy_value.c` (shared through `nocopy_value_int.h`, the object and reference walkers kept `noinline`) cost nothing:
`ab.sh write`, best of 7, copy / zero: all within ±2.5% (store object -1.9 / -2.5%, store+delete int, untouched, -2.0 / -2.2%); `bench/objects.php`, best of 5: 100 objects +2.9 / +5.9%, one object -2.2 / 0%.
Built as three, `nocopy_*` stopped inlining the store: fetch miss -5%, failing add -6%; as one, `ab.sh` read/write within noise.

**The arena as modules (`arena/`).** One header per concern (`proc`, `region`, `pool`, `index`, `log`, `policy`, `atoms`, `leases`, `shard`, `layout`),
headers ordered in layers and checked by `./dev layers`; the layers above see only `arena/nocopy_arena.h` and `nocopy_*` names (entry fields through accessors, writes through `nocopy_put_req`).
Kept from before, for speed: process-local globals rather than a handle passed to every call, the read path inline in the public header, the clock read inside the lock
(an injectable clock, not an explicit `now`, which would cost every fetch a clock read). `./dev ab main 7`, best of 7, copy / zero: fetches and stores within ±5%;
ttl churn's `evacuated` read 8-14% higher at best-of, but per run old copy 4959-5600 MB and new 5275-5687 MB (medians 5390 / 5362): noise, the merge code is unchanged.

**LTO in shared builds** (`--disable-nocopy-lto` turns it off), which inlines the arena into the fetch and store paths. `ab.sh` best of 5,
copy / zero: fetches +1-4%, store int +4%, inc +10%, store+delete +5-6%, failing add +5-7%. `nocopy_fetch_multi` stays out of line:
inlined into `nocopy_get_multiple`, copy-mode `fetch 10 keys` went 131 -> 151 ns.

**The layout id is the loaded code's build id, read in MINIT** (was: a hash of the `.so` file, read when the arena is
opened; before that the GNU build id, dropped for ~50 lines). The file hash read whatever file was at the path at open
time: a root fpm master's workers open on their first request, so after a package upgrade one pool ran two caches. The
GNU build-id note (`-Wl,--build-id`) or `LC_UUID`, from memory; the file hash only without one, and then in MINIT. Test
099: the same code from two different files shares one arena (fails with the file hash). Startup back: 400 interleaved bare
`php -r ';'` starts, median 6835 µs (file hash) -> 6516 µs, PHP alone 6397.

**The creator picks a random base** (was: the first free of two fixed addresses): the pointers in the arena were the same
on every host. Attachers already
mapped at the header's base, so nothing else changed; 2 MB-aligned in a 32 TB window, else a 192 GB one for 39-bit address
spaces. An attacher that has something at that address gets no cache, as before with the fixed one.

## Code style

Code comments are one short line, or none.

## Measured and rejected

| Idea | Result |
|---|---|
| `MADV_HUGEPAGE` on the arena | acts only with `shmem_enabled=advise` (default `never`); then 100k distinct keys -3.6/-6.6%, parallel fetches 2-4% slower |
| Asymmetric fence (`membarrier`) for pins | reader fence hides under cache misses anyway (shuffled 40.9 -> 41.3 ns); the writer pays 6-46 µs under the shard lock with 4-16 processes |
| Robust pthread mutexes to detect dead lock holders | 15% slower uncontended, 2-8x with 2-16 contending processes. Owner-id locks + `shard_repair` instead |
| Ghost variants | 2-way: no better. 2x tags: +0-3 points on mixed loads for 4 B/slot more |
| Dropping `nocopy_get_multiple`/`put_multiple` (PHP loop instead) | get 10 keys 142 -> 292 ns, 100 keys 1.3 -> 2.8 µs; put 100 keys 8.3 -> 9.3 µs |
| Dropping `nocopy.serializer=igbinary` (~50 lines) | the RESP tier serializes every non-integer value. Loopback, 2000 values, best of 5, put / cold get µs and bytes in RESP, php -> igbinary: 1k rows 260 / 377 -> 236 / 182, 141 -> 47 KB; 100 objects 37 / 57 -> 26 / 36, 15.5 -> 4.6 KB; render-array-like nested 1.5 KB of 3.7; 10 rows 1374 -> 490 B, times within noise; a 10 KB string unchanged |
| Compressing the RESP tier's payloads (LZ4, zstd) | `bench/compress.php` + `bench/codecs.c` (liblz4 1.10, libzstd 1.5.7, aarch64, random-word values: pessimistic), igbinary -> +LZ4 / +zstd 1, bytes and decode µs: render array 20.5 -> 14.4 / 8.9 KB, +1.1 / +9.9; 1k rows 77 -> 61 / 40 KB, +9 / +51; 20 KB HTML 24 -> 11 / 7.3 KB, +1.3 / +8.7; < 100 B and random bytes: nothing. Encode +60-100% (LZ4), +100-200% (zstd); zstd 3 no smaller than 1. igbinary already takes most (php -> igbinary 2.0-2.3x on arrays, faster decode); the rest pays only with RESP short of memory or on links of ~10 Gbit or slower. serialize+LZ4 instead of igbinary: 10-30% smaller, decode 1.6-3x slower |
| Arena blocks as the RESP encoding (spike: memcpy + rebase instead of unserialize + put) | fill 4-7x cheaper (render array 34 -> 7.3 µs, 1k rows 173 -> 23), store cheaper (the local put built the block), but 5-6x igbinary's bytes (102 KB vs 20.5): more transfer than CPU saved at 10 Gbit. +LZ4: 1.3-1.9x igbinary's bytes, fill 2-3.4x cheaper, store 2.3x slower; +zstd 1: igbinary's size, fill no cheaper. Needs a layout id and tagged out-of-block pointers (`ARENA_EMPTY`), as OPcache's file cache does (offsets from the buffer, interned strings with the low bit set, empty hashes by flag; `system_id` + Adler-32, no pointer checks) |
| Compressing arena entries | every hit decompresses (render array: LZ4 10 µs, zstd 27 µs, vs zero-copy 0); blocks shrink 3-4x (LZ4) / 5-6x (zstd 1). Only conceivable for cold entries instead of evicting them; shared strings across entries ([Writes](#writes)) come first |
| Dropping the frameless `nocopy_get`/`nocopy_put` (~55 lines with arginfo) | `./dev ab HEAD 5 read write`, copy / zero: fetch int 11.0 -> 15.2 / 11.0 -> 14.7 ns, fetch string 10KB 13.1 -> 18.0 / 11.2 -> 16.7, fetch int 100k distinct keys 27.8 -> 34.8 / 28.2 -> 34.9; stores 1-3 ns slower |
| A leaner Symfony read path, as done for Drupal's `get()` | nothing of that size is left: `./dev sfget`, bare `nocopy_get` 22 ns, `get()` hit 200, `getItem` hit 382 / miss 336, `hasItem` 183, `getItems` of 10 per item 298. The rest is `AbstractAdapter`'s (`getId`, item objects, generators) around private state (`$deferred`, `$ids`, the namespace version): overriding `getItem` would ignore `saveDeferred()`, an id cache of our own would have to follow every version change. In our code, measured apart: the closure `get()` builds per call 54 ns (kept in a property instead, a nested `get()` from an unserialize hook could run another key's callback), `doFetch`'s `ini_set` pair 44 ns (turns a missing class into a miss), `nocopy_get` for one id instead of `nocopy_get_multiple` 18 ns |
| Folding `nocopy_shared_with()` into `nocopy_stats()` | 15 -> 430 ns, and Drupal calls it on every write |
| Dropping `nocopy.entries_hint` for 1 slot per 128 B (was 256) | `fetch int, 100k distinct keys` 30.3 -> 39.2 ns (-22%), 500 B values -0.6/-1.1 hit points, 4.7% of the arena more in tables; only tiny entries gain (+7-9 points) |
| GL-Cache's utility per age (reads / age) to rank merge victims | loops collapse: 1.5x loop 25 -> 0% (young unread segments evicted first); a read prior does not fix it. Reads alone + forced oldest kept |
| Scanning the candidates' entries to rank them (no per-segment counter) | same hit ratios, but puts under eviction +40% at 32M, +75% at 256M (32 segments walked under the lock) |
| Cost-benefit order for compaction victims (LFS/RAMCloud `(1-u)·age/(1+u)`) | no change in any `hitratio.php` workload: rarely more than 4 sparse candidates |
| `nocopy_worker` aligned to 128 B (Apple's line size; one worker's per-fetch `hits` write shares a line with its neighbour's) | Graviton4 (c8g.xlarge, 64 B lines), 4-8 readers on 1 / 100k keys: all within ±2%, medians of 8. The +20-50% on an M3 Pro (128 B lines) sat inside its 2-3x run-to-run noise. Padding `nocopy_seg` to 64 B and dropping reader `hot` writes: no signal above that noise either |
| RESP: no refetch after our own write, via `CLIENT TRACKING ON REDIRECT <listener> BCAST PREFIX p NOLOOP` on every command connection | Valkey 8.1 and 9.1 send one push per tracking connection, also when they redirect to the same client: 3 redirecting workers gave the listener 3 pushes per foreign write and 2 per own write (`NOLOOP` skips only the writer). With 32 fpm workers that is 32 pushes per write per server, which the single listener exists to avoid. Skipping our own echo in the listener instead is unsafe: a cycle's invalidations of one key are merged, so an echo can hide another server's write |
| One-pass `copy_out` | `zend_array_dup` of an immutable array is already one memcpy; the cost was per-key allocation (fixed by key dedupe) |
| Persist into a private buffer, then `memcpy` + `nocopy_rebase` into the arena (no sizing pass, no fill outside the lock) | adopted 2026-11-12 with a relocation list instead of the rebase walk, see [Writes](#writes). Bounded then with two `docker/ab.sh` variants, `write`, best of 5: 1k rows +27.1 vs -17.9 µs, 10 rows +276 vs -183 ns, int +7.1 vs -2.4 ns, so writes ~15-20% slower; the rebase walk and a fresh buffer per write were most of it |
| Skip `size_zval` for small values: allocate a top-level lower bound under the lock, grow into the segment's free tail up to `NOCOPY_INLINE_PUT`, persist with bounds and type checks, trim (or give back and take the sized path) | arrays that fit gain (bench rows, best of 3 x 200k puts: 1 row 90-96 -> 83-87 ns, 2 rows 152-164 -> 121-126); nested arrays whose top level looks small but that overflow 2 KB pay a wasted persist (4 rows 244-268 -> 420-624, 6 rows 349-407 -> 531-643; `ab.sh` store array 10 rows -18/-23%); the checks in `persist_*` cost the sized path 3-8% (1k rows); scalars -5-9% (their sizing is ~2 ns). Always reserving 2 KB instead lost a hot key in test 029 (wasted tails evict sooner) |
| An upper-bound `size_zval` without key dedupe (then trim), to make sizing cheaper | the dedupe lookups are 5-25% of sizing (`ab.sh` `write`, a second sizing pass with and without them: 10 rows ~47 of ~187 ns, 1k rows ~4.5 of ~20 µs, unserialized keys ~1 of ~20 µs); the walk itself is the cost. At most ~8% of an array write, for blocks that overshoot |
| Shrinking `nocopy_fetch`'s frame (perf: ~20% of a zero-copy hit in its prologue and epilogue; 224 B frame, all callee-saved registers) | copy mode out of line: copy fetches of strings and arrays -6-9%, zero mode unchanged. Pin's new-pin path and `drop_transient` out of line: frame 224 -> 208 B, zero mode within noise. The pressure is the probe itself |
| Lazy ghost objects on fetch (`zend_object_make_lazy`, slots filled by the initializer on first access) | best case bounded with a prototype that never fills them (100 plain objects, `get` loop, 5 runs): kept copies 5.89-6.25 vs 5.90-6.30 µs untouched, `local_size=0` 15.6-18.0 vs 15.7-18.4 µs; making an object lazy costs what filling its slots saves, and touching every object (`isset`, empty initializer) was no faster either, so a real initializer (call + the same fill) only adds. Creating and freeing the objects is the cost, not filling them |
| One array walker (`array_walk(a, key_fn, val_fn, ctx)`, forced inline so each caller gets its own copy) for sizing, persisting, rebase, copy-out, graph spines and templates | callbacks inlined, fetches within noise; array stores slower: interleaved, best of 5 vs the hand loops, 10 rows 581 -> 591 ns, 1k rows 58.4 -> 58.9 us, 1k unserialized keys 64.3 -> 66.0 us (-2.7%; -5.7% with the walk state in `sizing`/`persist` instead of a local struct). No fewer lines either: each walk needs its callbacks |
| Native cache tags (per-entry tag versions checked in C on fetch, `nocopy_invalidate_tags`) | bounded, not built: Drupal's checksum path (one `nocopy_get_multiple` of the request's tag tokens + the PHP sum) vs the bare lookup as native's floor, best of 5 x 20k: 10 tags 728 vs 125 ns, 50 tags 3.3 vs 0.67 us, 200 tags 13.1 vs 2.7 us. At most 3-10 us of a ~660 us anonymous page-cache hit per worker (0.5-1.5%), less logged in. Drupal already batches tags into one lookup; pool/bin clears are O(1) version keys in both integrations |
| A sleeping (futex) shard lock for the contended far tail (`./dev tail` contend, 8 workers: get / put p99.99 2.5 / 2.3 ms) | not built: readers take no lock and have the same tail, and a control without any cache (8 processes timing a ~7 µs pure-PHP op in the same VM, 11 vCPUs) has p99.99 1.42 ms at 8 workers, 0.14 ms at 4. The tail is the VM's scheduler; it needs bare metal to say more |
| More C work on the request path, judged by a Drupal profile (`PERF_ONLY=1 CONFIGS=nocopy-all ./dev drupal-bench`, 2026-11-11, node page) | `nocopy.so` is 0.30% of all samples anonymous (php-fpm binary 28.1%, idle 52.9%: `ab` and nginx bound) and under 0.4% logged in (php-fpm 53.7%, `execute_ex` alone 13.6%); its top symbols `nocopy_fetch_multi` 0.16-0.17%, `graph_build` 0.17%. Nothing in the extension can move these pages by 1% |
| Copy mode's keep rule in one place (`graph_copy_fetch` asking `local_slot`, which then drops its type check, instead of repeating `local_ok && size <= local_size`) | `./dev ab HEAD 7 read`, rows that never reach it: fetch int -2.1 / -2.0%, fetch miss -2.9 / -2.6%, 1k rows -1.4 / -1.8% (twice; HEAD against itself: within ±0.8%). `graph_copy_fetch` kept out of line instead: string 100B -5.2 / -6.6%, 1k rows -6.3% copy. One line saved does not pay for either: the duplicate stays |

## Ruled out by the 2026-11 audit

**A dead worker's slot is ignored, not cleaned by others** (was: a reclaimer cleared its pins, a rotation its announced generation): the clear raced with the slot's next owner. The owner id found dead is remembered per process, so later reclaim passes cost no lock query; `pinned_segments` counts the stale bits until the slot is claimed.

## Adding to this page

A change that rests on performance gets a line here: what was decided, the numbers, and the price. A measured idea that
did not pay off goes into the table, so nobody measures it again.
