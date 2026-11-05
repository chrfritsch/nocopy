<div align="center">

# nocopy

**A shared-memory cache for PHP that hands out cached arrays without copying them.**

Lock-free reads, segment-based eviction, stampede-safe `nocopy_remember()`, one cache for php-fpm and the CLI,
and Valkey as a shared tier for several servers. An `apcu_*` polyfill (PHP) eases migration.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![PHP 8.4 | 8.5](https://img.shields.io/badge/PHP-8.4%20%7C%208.5-777bb4.svg)
![NTS and ZTS](https://img.shields.io/badge/NTS%20%2B%20ZTS-FrankenPHP-555.svg)
![Linux amd64 | arm64](https://img.shields.io/badge/Linux-amd64%20%7C%20arm64-555.svg)

</div>

```php
nocopy_put('products', $rows);      // 1,000 rows
$rows = nocopy_get('products');     // APCu: ~200 µs to rebuild the array. nocopy: ~13 ns, any size.
```

> **Status:** works and is tested, not released yet: build from source ([status and roadmap](docs/status.md)).

## Why

APCu copies every value out of shared memory on each fetch: arrays are rebuilt element by element, so the cost grows with
the size of what you cache, and readers take a lock. nocopy stores arrays and strings as opcache-style immutable structures
in shared memory and hands out a pointer: no copy, no unserialize, no lock ([how it works](docs/explanation/how-it-works.md)).

| | APCu | nocopy |
|---|---:|---:|
| fetch array, 1k rows | 202 µs | **13 ns** |
| fetch int / 100 B string | 28.6 / 34.9 ns | 13.0 / 13.0 ns |
| parallel fetch int, 8 workers | 3.8 M ops/s | **221 M ops/s** |
| Symfony app, cache-heavy page (php-fpm) | 4,116 req/s | **14,231 req/s** |
| Drupal 11, anonymous `/` / logged-in node page | 4,376–5,478 / 798–915 req/s | **8,679–9,934 / 1,191–1,313 req/s** |
| Drupal 11, database statements per logged-in request | 29–31 | **8** |

Linux, Docker on arm64, PHP 8.5. Pages that barely touch the cache gain nothing; the gain is where requests read large
cached arrays. All numbers and how to reproduce them: [benchmark results](docs/reference/benchmarks.md).

## Features

- **Zero-copy reads** under php-fpm, CGI, Apache, LiteSpeed, `php -S` and FrankenPHP classic mode. Long-running workers and the CLI get a safe **copy mode** that still reuses private copies of unchanged entries.
- **Lock-free reads, sharded writes.** Up to 8 shards with their own locks. A writer killed with `kill -9` mid-write is detected and its shard repaired.
- **Smarter eviction when full.** APCu wipes the whole cache. nocopy evicts its oldest segments and keeps entries read since they were written, using Segcache-style merging with a ghost list for keys that return.
- **Stampede protection.** `nocopy_remember()` runs the callback once per key across all workers and refreshes early via XFetch.
- **Symfony bundle** with `cache.adapter.nocopy`, which stores values without Symfony's marshaller so fetches stay zero-copy.
- **Valkey tier** (`nocopy.valkey`): Valkey becomes the cache of record shared by all servers, nocopy the zero-copy local copy in front of it. nocopy.so speaks RESP3 itself (no phpredis) and keeps local copies in sync through Valkey's client-side tracking.
- **Drupal module** with a cache backend for all bins and cache tag checksums, shared by php-fpm and Drush (`nocopy.shm_name`, or `nocopy.valkey` for several servers).
- **APCu polyfill** ([`apcu/`](apcu)): `apcu_*` in PHP on top of `nocopy_*`, for code you cannot change yet ([migrate from APCu](docs/how-to/migrate-from-apcu.md)).

## Quick start

Try it without installing anything but Docker:

```sh
git clone <this repository> nocopy && cd nocopy
./dev shell
php -r 'nocopy_put("a", [1, 2, 3]); var_dump(nocopy_get("a"));'
```

On a server (Linux, PHP 8.4+ with `phpize`, a C compiler):

```sh
phpize && ./configure && make
```

```ini
; conf.d file read by php-fpm and the CLI
extension=/path/to/nocopy/modules/nocopy.so
nocopy.shm_name=/nocopy       ; required: one cache for php-fpm and the CLI, kept over restarts
nocopy.shm_size=128M
```

```php
nocopy_put('products', $rows, ttl: 300);
$rows = nocopy_get('products', $hit);
$report = nocopy_remember('report', fn () => buildReport(), ttl: 60);   // computed once across all workers
```

## Documentation

| Start here | Then |
|---|---|
| [Your first cache](docs/tutorials/first-cache.md): a 10-minute tutorial | [Install on a server](docs/how-to/install.md), [php-fpm + CLI](docs/how-to/share-between-fpm-and-cli.md), [Valkey](docs/how-to/valkey.md) |
| [Functions](docs/reference/functions.md), [configuration](docs/reference/configuration.md), [`nocopy_stats()`](docs/reference/stats.md) | [Symfony](docs/how-to/symfony.md), [Drupal](drupal/README.md), [migrate from APCu](docs/how-to/migrate-from-apcu.md) |
| [How nocopy works](docs/explanation/how-it-works.md) | [Design decisions](docs/explanation/design-decisions.md), [architecture](docs/explanation/architecture.md) |

Everything: [docs/](docs/README.md). Project state and roadmap: [status](docs/status.md).

## Contributing

Bug reports and pull requests are welcome. For a bug, include the PHP version, SAPI (fpm, FrankenPHP, CLI…), NTS/ZTS and
the output of `nocopy_stats()`.

All you need is Docker: `./dev test` builds and runs the suite, `./dev ci` runs what CI runs. Start with the
[first change tutorial](docs/tutorials/first-change.md), then [Develop, test and benchmark](docs/how-to/develop.md).

## License

[MIT](LICENSE) © 2026 Christian Fritsch.
Eviction design inspired by [Segcache](https://www.usenix.org/conference/nsdi21/presentation/yang-juncheng) (Yang et al., NSDI 2021); the polyfill follows [APCu](https://github.com/krakjoe/apcu)'s API.
