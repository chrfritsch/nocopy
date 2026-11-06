# Benchmark results

Linux in Docker on arm64, PHP 8.5, release builds. Reproduce with `./dev bench`, `docker/fpm-bench.sh` and `./dev drupal-bench`
([how](../how-to/develop.md#benchmark)). Numbers on a dedicated x86_64 box are still to come ([status](../status.md)).

## Micro benchmarks (`./dev bench`)

| | APCu | nocopy (copy) | nocopy (zero-copy) |
|---|---:|---:|---:|
| fetch int / 100 B string | 28.2 / 34.4 ns | 11.6 / 19.5 ns | 11.0 / 11.5 ns |
| fetch array, 1k rows | 200 µs | 13 ns (kept copy) | **12 ns** |
| store int / 100 B string | 56.5 / 81.3 ns | 27.2 / 28.9 ns | same as copy |
| parallel fetch int, 8 workers | 3.7 M ops/s | 266 M ops/s | **350 M ops/s** |

2026-11-06. Copy mode reuses a kept private copy of an unchanged entry (`nocopy.local_size`): that is what the bench's
repeated fetch measures; the first fetch of 1k rows costs ~68 µs.

**Cross-check on Graviton4** (AWS c8g.xlarge, Kubernetes pod capped at 3 vCPU, PHP 8.5.12, same `docker/bench.sh`, 2026-11-05):
absolute times are higher for both, the ratios hold.

| | APCu | nocopy (zero-copy) | ratio here / above |
|---|---:|---:|---:|
| fetch int | 95.1 ns | 22.6 ns | 4.2x / 2.2x |
| fetch array, 1k rows | 264 µs | 22.9 ns | ~11,500x / ~15,500x |
| fetch array, 10k rows | 2.96 ms | 22.9 ns | size still irrelevant |
| parallel fetch int, 8 workers | 3.6 M ops/s | 104 M ops/s | 29x / 58x (3 vCPU cap) |
| 4 readers + 1 writer, reader throughput | 8.1 k ops/s | 1.63 M ops/s | |

## Symfony (`docker/fpm-bench.sh`)

`bench/sfapp` behind nginx + php-fpm, 8 static workers, `ab`, median of 3, cache pool without Symfony's marshaller
(2026-11-05).

| req/s | APCu | nocopy (copy) | nocopy (zero-copy) |
|---|---:|---:|---:|
| cache-heavy page (`products?page=3`) | 3,815 | 4,777 | **8,924** |
| cache-heavy page (`product/77`) | 3,595 | 4,626 | **9,182** |
| no cache (`hello`) | 9,386 | 9,886 | 9,238 |
| validator (`validate`) | 5,431 | 4,874 | 5,096 |

Zero-copy brings cache-heavy pages up to what a page without cache reaches on this host (~9,400 req/s), so the ratio is
capped by the host, not by nocopy. Pages that barely touch the cache gain nothing. An earlier run measured 4,116 vs
14,231 req/s on a faster day of the same host; it could not be reproduced and is retired.

## Drupal 11: where the gain comes from (`./dev drupal-bench`)

2,000 nodes, php-fpm 8 workers, MariaDB, Valkey on the same host; median of 3 interleaved runs on 2026-11-06
([raw numbers](../../bench/results-drupal/2026-11-06)). igbinary in every config except stock, nocopy configs load the `apcu_*`
polyfill as the Drupal module does. Each row changes one thing against the one above it:

| | anonymous `/` | logged-in `/` / node | DB statements anon / logged-in | first request after fpm restart | Drush save visible on the web |
|---|---:|---:|---:|---:|:---:|
| stock: DB bins, APCu fast layer | 3,814 req/s | 886 / 702 req/s | 7 / 29-31 | 219 ms | yes |
| apcu-all: all bins in APCu, tags in DB | 5,561 req/s | 1,076 / 933 req/s | 5 / 12-13 | 605 ms | **no** |
| nocopy-all-dbtags: all bins in nocopy, tags in DB | 7,839 req/s | **1,200 / 1,050 req/s** | 5 / 12-13 | 192 ms | yes |
| nocopy-all: bins and tags in nocopy | **10,929 req/s** | 1,133 / **1,050 req/s** | **0 / 7** | 188 ms | yes |

- **Bins out of the database** (stock → apcu-all): +46% anonymous, +21% / +33% logged-in. Any memory cache gets this.
- **No copy on read** (apcu-all → nocopy-all-dbtags): +41% anonymous, +12% / +13% logged-in.
- **Tags out of the database** (nocopy-all-dbtags → nocopy-all): +39% anonymous (the page cache hit needs no query at
  all); logged-in the same within the noise (-6% / 0%).
- apcu-all is only a measuring point: Drush and cron write to their own APCu, so their saves never reach php-fpm, and
  the cache starts empty after every fpm restart. What nocopy adds over it is one cache for every process that survives
  restarts, which is what makes moving all bins off the database safe.

Editor loop under reader load (p50; save / edit form / view after save) and readers served meanwhile:

| | stock | apcu-all | nocopy-all-dbtags | nocopy-all |
|---|---:|---:|---:|---:|
| editor | 31 / 50 / 43 ms | 30 / 35 / 22 ms | 28 / 33 / 21 ms | **23 / 33 / 21 ms** |
| readers | 497 req/s | 626 req/s | 691 req/s | **730 req/s** |

Without the `apcu_*` polyfill Drupal's class loader and FileCache find no `apcu_fetch()` and Composer checks the file system
for every class (strace: 807 `faccessat` per logged-in request, 65 with the polyfill, 68 with APCu). nocopy-all without and with
it, median of 3 interleaved runs (req/s): logged-in `/` 986 -> 1,290, node 1,042 -> 1,266, term 1,039 -> 1,326; anonymous `/`
8,536 -> 11,568, node 9,180 -> 12,803, term 10,551 -> 12,159.

The host ran ~20% slower than on 2026-11-05 (stock 3,814 vs 4,980 req/s anonymous), so compare within a run. A full run of all
ten configs the same day ([full](../../bench/results-drupal/2026-11-06/full)) caught nocopy-all in a slow window (4,247 req/s),
which the three runs did not reproduce.

Not in the table (full run, stock there 4,221 / 906 req/s): stock + igbinary is the same as stock (4,407 / 1,009 req/s), and so
is nocopy as chainedfast's fast layer (`nocopy-fast` 4,454 / 997 req/s, with `routes` 4,574 / 980 req/s). Chainedfast asks the database for each bin's
last write on every request, and those bins were never the bottleneck.

### Memory per php-fpm worker

`MEM_ONLY=1 CONFIGS="stock apcu-all nocopy-all redis nocopy-valkey" ./dev drupal-bench`: 8 workers, after warm-up and
400 anonymous + 400 logged-in requests to each of 3 pages, from `/proc/<pid>/smaps_rollup`. Private = pages only that
worker holds (its PHP heap, copies of cached values); PSS adds its share of the pages it shares (opcache, the cache's
shared memory). Two runs, equal to 0.1 MB.

| Config | Private MB / worker | PSS MB / worker | PSS, 8 workers | Valkey RSS |
|---|---|---|---|---|
| stock (APCu for 3 bins, the rest in MariaDB) | 7.2 | 14.2 | 113.5 | |
| apcu-all | 5.6 | 13.4 | 107.5 | |
| redis (drupal/redis 1.11) | 5.1 | 12.1 | 97.1 | 14.5 (+6.3 data) |
| nocopy-all | 4.1 | 11.5 | 92.3 | |
| nocopy-valkey | 4.2 | 11.6 | 93.1 | 15.8 (+7.6 data) |

nocopy keeps 1.5 MB less private memory per worker than APCu and 1 MB less than drupal/redis: fetched values are not
copied into the worker's heap. Small next to opcache and Drupal's own heap: these pages use ~3.5 MB of PHP memory per request
(php-fpm's `last request memory`, in 2 MB chunks), so the cache's copies are a part of that, not a multiple.

## Drupal 11, several servers: nocopy + Valkey vs drupal/redis

Same runs. All bins and tags shared through Valkey on the same host. Baseline: drupal/redis 1.11 with phpredis and
igbinary, APCu chainedfast in front of bootstrap/config/discovery.

| | drupal/redis | nocopy + Valkey |
|---|---:|---:|
| anonymous `/` | 4,515 req/s | **9,958 req/s** |
| logged-in `/` / node | 638 / 608 req/s | **1,292 / 1,100 req/s** |
| editor: save / edit form / view after save (p50) | 28 / 42 / 32 ms | **23 / 34 / 22 ms** |
| readers served during editing | 390 req/s | **727 req/s** |
| first request after fpm restart | 202 ms | 196 ms |
| second server joining: first request / p50 of requests 6-30 | 209 / 8.4 ms | 222 / **5.3 ms** |

Both have 0 / 7 database statements per anonymous / logged-in request: the difference is reading local zero-copy copies
instead of a network round trip and unserialize per item. drupal/redis 2.0-alpha2 is no faster than 1.11 (full run: 2,826 / 636
vs 2,750 / 532 req/s). A web save on server A and a Drush save both show up on server B in both setups.

## Batches (`nocopy_get_multiple` / `put_multiple`), one host

| | PHP loop of single calls | `*_multiple` |
|---|---:|---:|
| get 10 keys | 292 ns | 142 ns |
| get 100 keys | 2.8 µs | 1.3 µs |
| put 100 keys | 9.3 µs | 8.3 µs |

## Hit ratio

`bench/hitratio.php` measures read-through hit ratios under Zipf and loop workloads (optionally next to a few hot big values, which load some shards more than others); results back the eviction choices in
[Design decisions](../explanation/design-decisions.md#eviction).

### Against APCu and Redis

`./dev hitratio`: the same request sequence against each store, each given the same memory (`nocopy.shm_size`,
`apc.shm_size`, Valkey 8.1 `maxmemory`, which uses the same eviction as Redis 7.2). 500-byte values, read-through
(get, put on a miss), hit % after warm-up. Footprint (`fp`) = distinct keys' bytes / memory, `ohw` = one-off puts per
request, `w` = share of hits rewritten.

| Workload | Memory | nocopy | APCu | Valkey LRU | Valkey LFU |
|---|---|---|---|---|---|
| Zipf, fp 1.5 | 32M | 94.6 | 86.7 | 96.1 | 96.2 |
| | 64M | 94.1 | 87.1 | 96.4 | 96.4 |
| Zipf, fp 1.5, 0.5 one-off puts | 32M | 89.8 | 72.5 | 80.6 | 91.7 |
| | 64M | 91.6 | 73.6 | 82.4 | 90.7 |
| Zipf, fp 3 | 32M | 87.2 | 78.0 | 87.7 | 89.1 |
| | 64M | 86.3 | 79.1 | 88.5 | 89.6 |
| Zipf, fp 1.5, 20% of hits rewritten | 32M | 91.4 | 86.7 | 96.1 | 96.2 |
| | 64M | 90.9 | 87.1 | 96.4 | 96.5 |
| Loop, fp 1.1 | 32M | 72.0 | 0 | 100 | 100 |
| | 64M | 76.2 | 0 | 100 | 100 |
| Loop, fp 1.5 | 32M | 28.8 | 0 | 48.9 | 63.9 |
| | 64M | 49.3 | 0 | 37.6 | 64.5 |

- **APCu** empties the whole cache when it is full (`apc.ttl=0`, the default): 8-64 expunges per run, and a loop over
  slightly more than fits never hits.
- **Valkey holds ~15% more entries** in the same memory: 32M filled with 500-byte values keeps 58,400 in Valkey,
  50,900 in nocopy (600-byte blocks, ~660 bytes each with the slot table and free segments). That is most of its lead on
  plain Zipf and why `loop 1.1` fits whole. `maxmemory` counts allocated bytes only: Valkey's RSS was 38-40 MB at 32M
  and 70-71 MB at 64M, the process and allocator overhead come on top.
- **Rewrites** cost nocopy 3 points: an overwrite leaves the old copy as dead bytes until its segment is compacted.
  APCu and Valkey free it in place.
- **One-off keys**: nocopy (ghost list, frequency-aware merge) is level with LFU and 9 points above LRU.
- Valkey is a network hop: 4-8 µs a request here over loopback TCP, against 0.2-0.7 µs for nocopy and APCu.
