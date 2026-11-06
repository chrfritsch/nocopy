# Benchmark results

Linux in Docker on arm64, PHP 8.5, release builds. Reproduce with `./dev bench`, `docker/fpm-bench.sh` and `./dev drupal-bench`
([how](../how-to/develop.md#benchmark)). Numbers on a dedicated x86_64 box are still to come ([status](../status.md)).

## Micro benchmarks (`./dev bench`)

| | APCu | nocopy (copy) | nocopy (zero-copy) |
|---|---:|---:|---:|
| fetch int / 100 B string | 28.6 / 34.9 ns | 15.5 / 19.9 ns | 13.0 / 13.0 ns |
| fetch array, 1k rows | 202 µs | 91 µs | **13 ns** |
| store int / 100 B string | 55.7 / 81.8 ns | 29.3 / 34.6 ns | same as copy |
| parallel fetch int, 8 workers | 3.8 M ops/s | 154 M ops/s | **221 M ops/s** |

Copy mode reuses a kept private copy of an unchanged entry (`nocopy.local_size`): fetching 1k rows the second time costs
12.8 ns instead of 68 µs.

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

2,000 nodes, php-fpm 8 workers, MariaDB, Valkey on the same host, one full run on 2026-11-05
([raw numbers](../../bench/results-drupal/2026-11-05)). igbinary in every config except stock. Each row changes one
thing against the one above it:

| | anonymous `/` | logged-in `/` / node | DB statements anon / logged-in | first request after fpm restart | Drush save visible on the web |
|---|---:|---:|---:|---:|:---:|
| stock: DB bins, APCu fast layer | 4,980 req/s | 994 / 909 req/s | 7 / 29-31 | 181 ms | yes |
| apcu-all: all bins in APCu, tags in DB | 7,361 req/s | 1,156 / 1,086 req/s | 5 / 12-13 | 490 ms | **no** |
| nocopy-all-dbtags: all bins in nocopy, tags in DB | 8,340 req/s | 1,218 / 980 req/s | 5 / 12-13 | 164 ms | yes |
| nocopy-all: bins and tags in nocopy | **11,870 req/s** | **1,249 / 1,107 req/s** | **0 / 7** | 160 ms | yes |

- **Bins out of the database** (stock → apcu-all): +48% anonymous, +16-19% logged-in. Any memory cache gets this.
- **No copy on read** (apcu-all → nocopy-all-dbtags): +13% anonymous, +5% / -10% logged-in, within the noise of one
  run. Drupal pages read few large arrays per request, unlike the Symfony page above.
- **Tags out of the database** (nocopy-all-dbtags → nocopy-all): +42% anonymous (the page cache hit needs no query at
  all), +3% / +13% logged-in.
- apcu-all is only a measuring point: Drush and cron write to their own APCu, so their saves never reach php-fpm, and
  the cache starts empty after every fpm restart. What nocopy adds over it is one cache for every process that survives
  restarts, which is what makes moving all bins off the database safe.

Editor loop under reader load (p50; save / edit form / view after save) and readers served meanwhile:

| | stock | apcu-all | nocopy-all-dbtags | nocopy-all |
|---|---:|---:|---:|---:|
| editor | 30 / 47 / 40 ms | 29 / 33 / 20 ms | 29 / 34 / 21 ms | **23 / 34 / 20 ms** |
| readers | 563 req/s | 716 req/s | 696 req/s | 699 req/s |

Not in the table: stock + igbinary is the same as stock (4,934 / 962 req/s), and so is nocopy as chainedfast's fast
layer (`nocopy-fast` 5,127 / 883 req/s, with `routes` 4,816 / 975 req/s). Chainedfast asks the database for each bin's
last write on every request, and those bins were never the bottleneck.

## Drupal 11, several servers: nocopy + Valkey vs drupal/redis

Same run. All bins and tags shared through Valkey on the same host. Baseline: drupal/redis 1.11 with phpredis and
igbinary, APCu chainedfast in front of bootstrap/config/discovery.

| | drupal/redis | nocopy + Valkey |
|---|---:|---:|
| anonymous `/` | 5,105 req/s | **12,244 req/s** |
| logged-in `/` / node | 688 / 628 req/s | **1,340 / 1,202 req/s** |
| editor: save / edit form / view after save (p50) | 26 / 40 / 28 ms | **23 / 34 / 21 ms** |
| readers served during editing | 447 req/s | **711 req/s** |
| first request after fpm restart | 170 ms | 158 ms |
| second server joining: first request / p50 of requests 6-30 | 161 / 6.8 ms | 167 / **4.2 ms** |

Both have 0 / 7 database statements per anonymous / logged-in request: the difference is reading local zero-copy copies
instead of a network round trip and unserialize per item. drupal/redis 2.0-alpha2 is slower than 1.11 here (3,323 / 702
req/s). A web save on server A and a Drush save both show up on server B in both setups.

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
