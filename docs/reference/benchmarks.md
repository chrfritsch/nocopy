# Benchmarks

How nocopy compares with APCu, OPcache files, Redis or Valkey and Drupal's database cache, and what that means for your app.
The complete runs, with every configuration and caveat, are in the [benchmark log](../internals/benchmarks.md).

## In short

- **Reads cost the same whatever the size.** An int, a 100-byte string and a 1,000-row array each take about 12 ns. APCu
  needs 28 ns for the int and 200 µs for the array, because it copies every element.
- **Many workers don't slow it down.** Reads take no lock: 8 workers read 350 million values per second, against 3.7 million
  with APCu.
- **Writes are 2-3x faster than APCu.**
- **Whole apps:** a cache-heavy Symfony page goes from 25% to about 100% of what the server can do for a page with no cache
  at all. Drupal 11 serves 1.3-2.6x the requests, mostly because every cache bin leaves the database.
- **No gain** for pages that barely touch the cache.

**Setup:** Linux in Docker on an Apple M3 Pro (arm64, 11 CPUs for the VM), PHP 8.5 with production settings, November
2026. The read results hold on AWS Graviton4. Numbers on dedicated x86_64 hardware are still to come. A VM adds noise, so
small differences (under ~5%) mean nothing.

## Single operations

Time per call, lower is better. "Copy mode" is how CLI scripts and FrankenPHP workers read; "zero-copy" is how php-fpm
requests read ([the two modes](../explanation/how-it-works.md#zero-copy-and-copy-mode)).

| | APCu | nocopy, copy mode | nocopy, zero-copy |
|---|---:|---:|---:|
| Read an int | 28.2 ns | 11.6 ns | 11.0 ns |
| Read a 100-byte string | 34.4 ns | 19.5 ns | 11.5 ns |
| Read a 1,000-row array | 200 µs | 13 ns ¹ | **12 ns** |
| Write an int | 55.3 ns | 27.3 ns | 27.3 ns |
| Write a 1,000-row array | 191 µs | 59 µs | 59 µs |
| Write an object | 329 ns | 174 ns | 174 ns |
| Increment a counter | 22.6 ns | 11.1 ns | 11.1 ns |

¹ A repeated read reuses the kept copy (`nocopy.local_size`); the first read of 1,000 rows in copy mode takes ~68 µs.

**With 8 workers at once**, in operations per second, higher is better:

| | APCu | nocopy |
|---|---:|---:|
| Reads (same int) | 3.7 M | **350 M** |
| Writes (each worker its own key) | 10.4 M | **141 M** |
| 4 readers while 4 writers write | 6.9 M | **134 M** |

**Reading several keys at once** with `nocopy_get_multiple()` is about twice as fast as a loop: 10 keys 142 ns instead of
292 ns, 100 keys 1.3 µs instead of 2.8 µs.

Reproduce: `./dev bench`.

## Symfony

A Symfony app behind nginx and php-fpm (8 workers), with the PHP settings from Symfony's performance guide. Requests per
second, higher is better. `hello` uses no cache at all: it is the most this server can do.

| req/s | APCu (raw pool, no marshaller) | nocopy (bundle) |
|---|---:|---:|
| Cache-heavy page (`products?page=3`) | 4,542 | **17,430** |
| Cache-heavy page (`product/77`) | 4,416 | **15,801** |
| No cache (`hello`) | 18,182 | 16,660 |
| Validator page (`validate`) | 9,347 | 9,047 |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="symfony-ceiling-dark.svg">
  <img alt="Symfony cache-heavy pages: nocopy 17,430 and 15,801 req/s, about all of an empty page (~17,000); APCu 4,542 and 4,416 req/s, 25% and 24%" src="symfony-ceiling.svg" width="720">
</picture>

**What it means:** with nocopy, a page that reads big cached arrays is about as fast as a page that reads nothing; with
APCu it runs at a quarter of that. Pages that barely use the cache (`validate`) are the same with both.

Reproduce: `./dev sfbench`.

### FrankenPHP worker mode

The same app, booted once per worker, 8 workers. nocopy runs in copy mode here and reuses its kept copies.

| req/s | APCu | PHP files (OPcache) | nocopy |
|---|---:|---:|---:|
| Cache-heavy page (`products?page=3`) | 3,608-3,864 | 15,899-16,320 | **16,140-17,308** |
| Cache-heavy page (`product/77`) | 3,559-3,777 | 17,634-18,208 | **17,113-17,325** |
| No cache (`hello`) | 17,354-18,111 | 17,588-18,282 | 18,396-18,560 |

**What it means:** keep `nocopy.local_size` large enough for your hot data. With it turned off (`0`), the cache-heavy pages
drop to 5,300-6,000 req/s, still 1.5x APCu.

Reproduce: `./dev fpbench`.

### Compared with OPcache files (`PhpFilesAdapter`)

Symfony can also cache values as PHP files that OPcache then serves without copying. On a page that only reads, both reach
the server's limit (about 14,500 req/s in that run). The difference is in single operations:

| | PHP file in OPcache | nocopy |
|---|---:|---:|
| Read a 1,000-row array | 52 ns | 13 ns |
| Read 1 of 1,000 small keys | 112 ns | 43 ns |
| Write a 1-row array | 32-39 µs | 0.11 µs |
| Write a 1,000-row array | 2.3 ms | 73 µs |
| OPcache memory wasted per rewrite of the 1,000-row array | 385 KB | 0 |

**What it means:** OPcache files suit data written once per deploy (config, routes, metadata). Every rewrite wastes OPcache
memory, and when too much is wasted OPcache restarts and recompiles the whole app. nocopy suits data written at runtime.

## Drupal 11

A Drupal site with 2,000 nodes, php-fpm with 8 workers, MariaDB, production PHP settings. Requests per second, higher is
better.

### One server

| req/s | Drupal as shipped | nocopy module |
|---|---:|---:|
| Anonymous front page | 4,671 | **12,145** |
| Logged-in front page | 797 | **1,365** |
| Logged-in node page | 835 | **1,122** |
| Logged-in JSON:API list of 20 articles | 1,200 | **2,084** |
| Database queries per request, anonymous / logged-in | 7 / 18-31 | **0 / 7** |
| Editor: save / edit form / view after save | 31 / 48 / 42 ms | **21 / 32 / 19 ms** |

### Where the gain comes from

Each step changes one thing:

| Step | Anonymous | Logged-in node |
|---|---:|---:|
| Drupal as shipped (database cache, APCu for 3 bins) | 4,671 | 835 |
| All cache bins in APCu | 5,883 (+26%) | 1,038 (+24%) |
| All cache bins in nocopy (no copying on read) | 8,009 (+36%) | 1,141 (+10%) |
| Cache tags in nocopy too | 12,145 (+52%) | 1,122 (-2%) |

**What it means:** moving the cache out of the database brings the most. That is only safe because php-fpm, Drush and cron
share one nocopy cache that survives restarts: with "all bins in APCu", a Drush save never reaches the web, and the cache
is empty after every php-fpm restart.

### Several servers, against drupal/redis

All bins and cache tags shared through Valkey (or Redis) on the same host. drupal/redis with phpredis and igbinary, and
APCu in front of the small bins, as recommended.

| | drupal/redis | nocopy + Valkey |
|---|---:|---:|
| Anonymous front page | 4,837 req/s | **12,538 req/s** |
| Logged-in front page / node / JSON:API | 642 / 634 / 1,159 req/s | **1,387 / 1,152 / 1,754 req/s** |
| Editor: save / edit form / view after save | 27 / 41 / 32 ms | **22 / 32 / 21 ms** |
| A second server joining: typical request after warm-up | 9.4 ms | **5.7 ms** |

**What it means:** both run the same database queries; nocopy reads local copies without a network round trip or
unserializing.

### Memory per php-fpm worker

| | Private memory per worker | PHP peak per request |
|---|---:|---:|
| Drupal as shipped | 5.2 MB | 3.5 MB |
| drupal/redis | 4.1 MB | 3.5 MB |
| nocopy | **3.5 MB** | **1.8 MB** |

Fetched values are not copied into the worker's memory, so each worker uses less. The cache itself used 19.7 MB of shared
memory after warm-up.

Reproduce: `./dev drupal-bench`.

## Response times under load

8 CLI workers time every read and write for 10 seconds. Microseconds, read / write; lower is better. p99 means 99% of
operations were faster.

| Scenario | | p50 | p99 | p99.9 |
|---|---|---:|---:|---:|
| Data fits in memory, 1% rewritten | APCu | 0.4 / 2.5 | 6.6 / 3,846 | 66 / 6,469 |
| | nocopy | 0.3 / 0.6 | **3.2 / 7.8** | **7.8 / 85** |
| 1.5x more data than memory, 20% rewritten | APCu | 0.6 / 1.1 | 28 / 1,617 | 58 / 4,016 |
| | nocopy | 0.5 / 0.6 | **4.1 / 48** | **10 / 194** |
| 4 readers and 4 writers on the same 64 arrays | APCu | 22 / 37 | 78 / 163 | 202 / 2,008 |
| | nocopy | 11 / 7.5 | **53 / 30** | 211 / 163 |

**What it means:** with APCu, writes wait for readers and take milliseconds at p99; nocopy's stay under 50 µs. Evicting
under memory pressure barely shows. At the extreme tail (1 in 10,000) both take 1-2 ms when 8 workers compete; the same
happens without any cache in this VM, so that is the VM's scheduler.

Many processes don't slow it down: with 1,000 idle processes attached, or 256 php-fpm workers, throughput stays the same.

Reproduce: `./dev tail`.

## Hit rate when memory is full

The same requests against each cache, each given the same memory, 500-byte values, read-through (get, and put on a miss).
Hit rate in %, higher is better. "1.5x" means the data is 1.5 times the memory.

| Workload (32 MB) | nocopy | APCu | Valkey LRU | Valkey LFU |
|---|---:|---:|---:|---:|
| Popular keys read more often (Zipf), 1.5x | 94.6 | 86.7 | 96.1 | 96.2 |
| The same, plus keys used only once | 89.8 | 72.5 | 80.6 | 91.7 |
| Zipf, 3x | 87.2 | 78.0 | 87.7 | 89.1 |
| Zipf, 1.5x, 20% of hits rewritten | 91.4 | 86.7 | 96.1 | 96.2 |
| All keys in a loop, 1.1x | 72.0 | 0 | 100 | 100 |

**What it means:**

- APCu empties the whole cache when it is full, so a loop over slightly more than fits never hits.
- Valkey fits about 15% more entries into the same memory, which is most of its lead. But each Valkey request is a network
  hop (4-8 µs here, against 0.2-0.7 µs for nocopy).
- Keys used only once don't push popular keys out of nocopy (it is level with Valkey's LFU, 9 points above LRU).
- Rewrites cost nocopy a few points: an overwritten value stays as dead space until its segment is compacted.

Reproduce: `./dev hitratio`.

## Long runs and large caches

- **50 minutes under heavy churn** (8 workers, data 2x the cache, ~370,000 evictions a second): hit rate, speed and memory
  were the same in the last 10 minutes as in the first. No leak, no fragmentation building up.
- **Large caches:** creating the cache costs ~65 ms per GB, once per server start; attaching costs nothing. Up to 4 GB,
  reads stay at about 4 million per second with 4 readers and 4 writers, and writes pause at most 26-42 ms while memory is
  being freed.

Reproduce: `./dev soak`, `./dev big`.

## Run them yourself

All benchmarks run in Docker through `./dev`; no PHP needed on your machine. The commands and their options:
[`./dev` commands](dev-commands.md) and [Develop, test and benchmark](../how-to/develop.md#benchmark).
