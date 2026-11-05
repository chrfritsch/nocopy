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

**Kept copies** (`nocopy.local_size`, 8M default): 1k rows 68 µs -> 12.8 ns, 10k rows 711 µs -> 12.8 ns, 10 KB string
182 -> 12.8 ns; small values at most 0.8 ns slower. **Kept on the second fetch only** (a doorkeeper): keeping every copy
made 100k distinct strings 37 -> 64 ns. Over budget, a SIEVE hand evicts copies not used since it last passed (was: a full flush). `bench/local.php`, 3000 arrays
of 20 rows under Zipf: local_size 4M 320-371 -> 233-242 ns, 1M 423-429 -> 333-366 ns; within budget and on loops unchanged.

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
| One-pass `copy_out` | `zend_array_dup` of an immutable array is already one memcpy; the cost was per-key allocation (fixed by key dedupe) |

## Adding to this page

A change that rests on performance gets a line here: what was decided, the numbers, and the price. A measured idea that
did not pay off goes into the table, so nobody measures it again.
