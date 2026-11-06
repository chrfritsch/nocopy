# Design decisions and rejected ideas

Why nocopy is built the way it is, with the measurements behind each choice, and what was tried and dropped. Read this
before proposing a change in these areas: the obvious alternative has often been measured already.

Numbers are from Linux in Docker on arm64 unless noted ([benchmarks](../reference/benchmarks.md)). History is in `git log`.

## Memory lifetime

**Per-worker pin bitmaps over segments.** A worker marks the segments it reads from; a segment is reused only when no
worker pins it. The first design, an epoch plus a per-entry retire list, piled up garbage; the old "zero-copy write is
50-100x slower" results came from it.

**Pins released only at request end.** Userland may keep any fetched value until then, so there is no earlier safe point.
Long-running processes therefore use copy mode.

## Sharing

**`nocopy.shm_name` is required; without it there is no cache.** Unset or not attachable means a startup warning and no
cache, never a private one: a private cache splits invalidation silently ([Sharing](sharing.md#all-or-nothing)).
**No default name:** two sites of one user would share keys.

**Valkey: one listener process per arena** holds the only tracking connection: 16 connections and 16 copies of every push
per server became 1. The writer's own server refetches its write once, since the listener cannot tell own writes apart.

**Valkey connections from vendored libvalkey** (`third_party/libvalkey`, 0.5.0), replacing our own socket, TLS BIO and RESP3 parser
(~600 lines and a fuzzer): Cluster, when it comes, is libvalkey's `cluster.c` rather than ours. Vendored because no distro or Homebrew
packages it and PIE builds from the release archive (submodules are left out). `bench/` has no Valkey A/B; a put/get/inc/get_multiple loop
over loopback (scratch script, 9 interleaved rounds, rounds where both builds ran at the VM's fast speed): put 10.3-11.4 -> 11.5-12.0 µs
(each reply is a malloc'd tree), get 3.9-4.0 -> 3.7-4.0, inc 11.8-13.8 -> 12.5-13.0, get_multiple of 100 58-75 -> 52-71. Price: ~7k lines
of C in the repo, updated with `scripts/vendor-libvalkey.sh <tag>`.

**Valkey tier behind a seam.** `nocopy.c` reaches it only through `nocopy_vk_*`, and `--disable-nocopy-valkey` compiles it
out, so the core builds and ships without it. CI runs the suite both ways. Default stays on.

## Hashing and slots

**Slot home from a mixed hash.** PHP's DJBX33A gives keys like `prefix.N` regular low bits; as home slots they clustered.
On one shard at 73% fill: hit 28.8 -> 17.8 ns, miss 58.3 -> 15.2 ns, delete+put 107 -> 29 ns. Price: `fetch int, 100k
distinct keys` 22.1 -> 26.8 ns (keys spread over more cache lines). Test 018 picks keys homed in the first half to keep
its race window.

**Fixed slot tables, not growable:** no stall while growing. Price: many tiny entries evict by count before memory is full;
raise `nocopy.entries_hint`.

**Keep `nocopy.entries_hint`** (see rejected ideas below for dropping it).

## Copy mode

**The fetch mode follows the SAPI; there is no setting.** `nocopy.request_scope` (`auto`/`1`/`0`) only let users get it wrong:
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

**Copy-out of `""` and 1-char strings uses PHP's interned ones** (`ZVAL_STRINGL_FAST`): no allocation. A cold copy of
`bench/run.php`'s 1k rows (two 1-char strings each, `local_size=0`) 69 -> 59 µs.

## Writes

Found with cachegrind (per-op cost from runs of n and 2n ops, `cg_diff`). **`persist_array` takes the next free index
from the loop it already runs** (was a second pass over every array, ~10% of a put), and **its key dedupe remembers the
source key**, so repeated keys match by pointer (was a `memcmp` against the persisted copy for every key). Store array
10 / 1k / 1k unserialized-key rows: 616 -> 520 ns, 59.2 -> 49.5 µs, 65.7 -> 53.8 µs. **`nocopy_get_multiple` sizes
its result up front**: no rehash, 10 keys 122 -> 109 ns.

**`nocopy_delete_multiple`**, because Drupal's `deleteMultiple()` and Symfony's `deleteItems()` deleted key by key, one `UNLINK`
round trip each with `nocopy.valkey`. `bench/delete.php`, Valkey on loopback (medians of 21): 10 keys 340 -> 39 µs, 100 keys
3.3 ms -> 75 µs, 1000 keys 33 -> 0.44 ms. Locally 100 keys 2.8 -> 1.9 µs, 1000 keys 33 -> 22 µs. The APCu polyfill keeps its loop:
`apcu_delete()` returns the keys that failed, and a count cannot say which.

## Eviction

**Segcache-style merge** instead of APCu's full wipe: the oldest segments are merged, read entries move forward, the rest go.

**No per-entry hit counters:** an exact counter is a shared write on every read. A Morris-style probabilistic counter
(0-15) writes a cache line about log2(reads) times between merges.

**Merge victims: the oldest segment plus the 3 least read of the 32 oldest** (after GL-Cache's group-level eviction), ranked by a
per-segment sum of the entries' read estimates kept up to date on every `hot` bump. `bench/hitratio.php`, 4M / 32M vs the 4 oldest:
Zipf 91.2 -> 92.2 / 92.9 -> 93.9, Zipf + 0.5 one-hit wonders 80.1 -> 88.0 / 83.2 -> 89.4, loop 1.1x 25 -> 49 / 25 -> 50, loop 1.5x 25 -> 25 / 18 -> 25;
20% overwrites 84.9 -> 84.0 / 87.3 -> 87.2. Puts under eviction and fetches unchanged. Always taking the oldest keeps scans from sticking.

**Ghost list (S3-FIFO):** one 32-bit tag per slot. Measured with `bench/hitratio.php`: no ghost loses 2 points on mixed
loads and all hits on scans.

**No `nocopy.ttl` default:** it expired Drupal's cache tag tokens. Callers pass `$ttl`.

**Drupal bin version in the item, not the key:** a versioned key orphaned every item on `deleteAll()`, and with
`nocopy.valkey` the TTL-less small bins (config, discovery, ...) then stayed in Valkey for good after each `drush cr`.
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

## APCu

**Compatibility as a PHP polyfill**, not in the extension. The extension keeps only `apc.enabled`/`apc.enable_cli`
(Symfony's `ApcuAdapter::isSupported()` and Composer check them). `bench/run.php` measures `nocopy_*` directly by
re-running itself with `apcu_*` rewritten (fetch int 11.7 ns zero-copy, without the polyfill's extra call).

**Objects are always stored serialized**, as in APCu, with no `$serialize` flag on writes. With the flag, every caller in
the repo (polyfill, Drupal, Symfony) passed `true`, and without it shared references were silently flattened into copies.

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
| Folding `nocopy_shared_with()` into `nocopy_stats()` | 15 -> 430 ns, and Drupal calls it on every write |
| Dropping `nocopy.entries_hint` for 1 slot per 128 B (was 256) | `fetch int, 100k distinct keys` 30.3 -> 39.2 ns (-22%), 500 B values -0.6/-1.1 hit points, 4.7% of the arena more in tables; only tiny entries gain (+7-9 points) |
| GL-Cache's utility per age (reads / age) to rank merge victims | loops collapse: 1.5x loop 25 -> 0% (young unread segments evicted first); a read prior does not fix it. Reads alone + forced oldest kept |
| Scanning the candidates' entries to rank them (no per-segment counter) | same hit ratios, but puts under eviction +40% at 32M, +75% at 256M (32 segments walked under the lock) |
| Cost-benefit order for compaction victims (LFS/RAMCloud `(1-u)·age/(1+u)`) | no change in any `hitratio.php` workload: rarely more than 4 sparse candidates |
| `nocopy_worker` aligned to 128 B (Apple's line size; one worker's per-fetch `hits` write shares a line with its neighbour's) | Graviton4 (c8g.xlarge, 64 B lines), 4-8 readers on 1 / 100k keys: all within ±2%, medians of 8. The +20-50% on an M3 Pro (128 B lines) sat inside its 2-3x run-to-run noise. Padding `nocopy_seg` to 64 B and dropping reader `hot` writes: no signal above that noise either |
| Valkey: no refetch after our own write, via `CLIENT TRACKING ON REDIRECT <listener> BCAST PREFIX p NOLOOP` on every command connection | Valkey 8.1 and 9.1 send one push per tracking connection, also when they redirect to the same client: 3 redirecting workers gave the listener 3 pushes per foreign write and 2 per own write (`NOLOOP` skips only the writer). With 32 fpm workers that is 32 pushes per write per server, which the single listener exists to avoid. Skipping our own echo in the listener instead is unsafe: a cycle's invalidations of one key are merged, so an echo can hide another server's write |
| One-pass `copy_out` | `zend_array_dup` of an immutable array is already one memcpy; the cost was per-key allocation (fixed by key dedupe) |
| Persist into a private buffer, then `memcpy` + `nocopy_rebase` into the arena (no sizing pass, no fill outside the lock) | bounded with two `docker/ab.sh` variants, `write`, best of 5: what it adds (persist privately + copy + rebase) costs more than what it saves (a second `size_zval`): 1k rows +27.1 vs -17.9 µs, 10 rows +276 vs -183 ns, int +7.1 vs -2.4 ns, so writes ~15-20% slower. A growable buffer also moves pointers already written |
| Skip `size_zval` for small values: allocate a top-level lower bound under the lock, grow into the segment's free tail up to `NOCOPY_INLINE_PUT`, persist with bounds and type checks, trim (or give back and take the sized path) | arrays that fit gain (bench rows, best of 3 x 200k puts: 1 row 90-96 -> 83-87 ns, 2 rows 152-164 -> 121-126); nested arrays whose top level looks small but that overflow 2 KB pay a wasted persist (4 rows 244-268 -> 420-624, 6 rows 349-407 -> 531-643; `ab.sh` store array 10 rows -18/-23%); the checks in `persist_*` cost the sized path 3-8% (1k rows); scalars -5-9% (their sizing is ~2 ns). Always reserving 2 KB instead lost a hot key in test 029 (wasted tails evict sooner) |
| An upper-bound `size_zval` without key dedupe (then trim), to make sizing cheaper | the dedupe lookups are 5-25% of sizing (`ab.sh` `write`, a second sizing pass with and without them: 10 rows ~47 of ~187 ns, 1k rows ~4.5 of ~20 µs, unserialized keys ~1 of ~20 µs); the walk itself is the cost. At most ~8% of an array write, for blocks that overshoot |

## Adding to this page

A change that rests on performance gets a line here: what was decided, the numbers, and the price. A measured idea that
did not pay off goes into the table, so nobody measures it again.
