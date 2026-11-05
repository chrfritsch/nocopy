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

## Symfony (`docker/fpm-bench.sh`)

`bench/sfapp` behind nginx + php-fpm, 8 static workers, `ab`, median of 3.

| | APCu | nocopy (copy) | nocopy (zero-copy) |
|---|---:|---:|---:|
| cache-heavy page | 4,116 req/s | 4,553 req/s | **14,231 req/s** |

Pages that barely touch the cache gain nothing; the gain is where requests read large cached arrays.

## Drupal 11, one server (`./dev drupal-bench`)

2,000 nodes, php-fpm 8 workers, MariaDB. Lowest–highest of 3 full runs, config order rotated between runs
([raw numbers](../../bench/results-drupal)).

| | stock (DB + APCu) | stock + igbinary | nocopy fast layer + routes | nocopy, all bins |
|---|---:|---:|---:|---:|
| anonymous `/` (page cache) | 4,376–5,478 req/s | 3,840–5,062 req/s | 3,784–5,383 req/s | **8,679–9,934 req/s** |
| logged-in node page | 798–915 req/s | 815–893 req/s | 979–995 req/s | **1,191–1,313 req/s** |
| logged-in front page | 966–1,031 req/s | 904–1,071 req/s | 876–1,035 req/s | **1,051–1,483 req/s** |
| editor: save / edit form / view after save (p50) | 27–31 / 44–48 / 36–42 ms | 27–30 / 43–49 / 37–41 ms | 26–29 / 43–47 / 35–38 ms | **20–22 / 32–33 / 19–21 ms** |
| readers served during editing | 530–598 req/s | 562–625 req/s | 598–651 req/s | **752–859 req/s** |
| database statements per logged-in request | 29–31 | 29–31 | 30–32 | **8** |
| database statements per anonymous request | 7 | 7 | 7 | **5** |

Statements include connection setup. These nocopy numbers predate the shared cache (an anonymous hit then paid a journal
query). Reruns with `nocopy.shm_name` and no journal, same setup:

| run | anonymous `/` | logged-in front / node | statements anon / logged-in | first logged-in request after fpm restart |
|---|---:|---:|---:|---:|
| 2026-11-05 a | 12,938 req/s | 1,467 / 1,283 req/s | 0 / 7 | 155 ms |
| 2026-11-05 b (noisy host) | 12,229 req/s | | – / 7 | 146 ms |

The **fast layer** (`ChainedFastBackend` with nocopy instead of APCu, `routes` added to `bootstrap`, `config`, `discovery`)
also works on several servers but stays close to stock (806–1,001 req/s logged-in node page without `routes`): chainedfast
asks the database for each bin's last write on every request, and those bins were never the bottleneck. The gain comes
from moving all bins and the cache tags off the database.

## Drupal 11, Valkey (`bench/results-drupal/valkey`)

Same site, all bins and tags shared, one run. Baseline: drupal/redis with phpredis on the same Valkey, APCu chainedfast in
front of bootstrap/config/discovery.

| req/s (p50) | drupal/redis | nocopy + Valkey | nocopy, single server |
|---|---|---|---|
| anonymous `/` | 5,129 (2 ms) | 12,546 (1 ms) | 9,109 (1 ms) |
| logged-in `/` | 727 (10 ms) | 1,437 (5 ms) | 1,454 (5 ms) |
| logged-in node | 711 (11 ms) | 1,299 (6 ms) | 1,239 (6 ms) |
| first logged-in request after an fpm restart | 157 ms | 182 ms | 504 ms |
| second server, requests 6-30 (p50) | 6.6 ms | 4.3 ms | n/a |

A web save on server A and a Drush save both show up on server B in both shared setups. The single-server column of this
run still paid the journal query (gone since), which is why Valkey beats it on anonymous traffic.

## Batches (`nocopy_get_multiple` / `put_multiple`), one host

| | PHP loop of single calls | `*_multiple` |
|---|---:|---:|
| get 10 keys | 292 ns | 142 ns |
| get 100 keys | 2.8 µs | 1.3 µs |
| put 100 keys | 9.3 µs | 8.3 µs |

## Hit ratio

`bench/hitratio.php` measures read-through hit ratios under Zipf and loop workloads; results back the eviction choices in
[Design decisions](../explanation/design-decisions.md#eviction).
