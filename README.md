<div align="center">

# nocopy

**A shared-memory cache for PHP that hands out cached arrays without copying them.**

A drop-in replacement for APCu (`apcu_*`, `APCUIterator`, `apc.*` INI) with a lock-free read path,
segment-based eviction, and stampede-safe `nocopy_remember()`.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![PHP 8.3 | 8.4 | 8.5](https://img.shields.io/badge/PHP-8.3%20%7C%208.4%20%7C%208.5-777bb4.svg)
![NTS and ZTS](https://img.shields.io/badge/NTS%20%2B%20ZTS-FrankenPHP-555.svg)
![Linux amd64 | arm64](https://img.shields.io/badge/Linux-amd64%20%7C%20arm64-555.svg)

</div>

```php
apcu_store('products', $rows);      // 1,000 rows
$rows = apcu_fetch('products');     // APCu: ~200 µs to rebuild the array. nocopy: ~13 ns, any size.
```

> **Status:** works and is tested in CI, not released yet. No PECL/PIE package and no Packagist entry yet: build from source.

## Why

APCu copies every value out of shared memory on each fetch: arrays are rebuilt element by element (or unserialized),
so the cost grows with the size of what you cache. Readers also take a lock, so throughput drops as workers are added.

nocopy stores arrays and strings as **opcache-style immutable structures** in one `mmap` arena that all php-fpm workers
(or ZTS threads) share. `apcu_fetch()` then returns a pointer straight into shared memory, with no copy, no unserialize and no lock.
Per-worker pins keep that memory alive until the request ends, so a fetched value can never point at freed memory.

Measured on Linux (Docker, arm64, PHP 8.5, release builds; details and method in [`HANDOVER.md`](HANDOVER.md)):

| | APCu | nocopy (copy) | nocopy (zero-copy) |
|---|---:|---:|---:|
| fetch int / 100 B string | 28.6 / 34.9 ns | 15.5 / 19.9 ns | 13.0 / 13.0 ns |
| fetch array, 1k rows | 202 µs | 91 µs | **13 ns** |
| store int / 100 B string | 55.7 / 81.8 ns | 29.3 / 34.6 ns | same as copy |
| parallel fetch int, 8 workers | 3.8 M ops/s | 154 M ops/s | **221 M ops/s** |
| Symfony app, cache-heavy page (php-fpm) | 4,116 req/s | 4,553 req/s | **14,231 req/s** |

Pages that barely touch the cache gain nothing. The gain appears where requests read large cached arrays.

**Drupal 11** (2,000 nodes, php-fpm 8 workers, MariaDB, one server; [module](drupal), [raw numbers](bench/results-drupal)).
Lowest–highest of 3 full runs, config order rotated between runs:

| | stock (DB + APCu) | stock + igbinary | nocopy fast layer + routes | nocopy, all bins |
|---|---:|---:|---:|---:|
| anonymous `/` (page cache) | 4,376–5,478 req/s | 3,840–5,062 req/s | 3,784–5,383 req/s | **8,679–9,934 req/s** |
| logged-in node page | 798–915 req/s | 815–893 req/s | 979–995 req/s | **1,191–1,313 req/s** |
| logged-in front page | 966–1,031 req/s | 904–1,071 req/s | 876–1,035 req/s | **1,051–1,483 req/s** |
| editor: save / edit form / view after save (p50) | 27–31 / 44–48 / 36–42 ms | 27–30 / 43–49 / 37–41 ms | 26–29 / 43–47 / 35–38 ms | **20–22 / 32–33 / 19–21 ms** |
| readers served during editing | 530–598 req/s | 562–625 req/s | 598–651 req/s | **752–859 req/s** |
| database statements per logged-in request | 29–31 | 29–31 | 30–32 | **8** |
| database statements per anonymous request | 7 | 7 | 7 | **5** |

Statements include connection setup. These nocopy numbers predate the shared cache (an anonymous hit then paid the journal query). One
rerun with `nocopy.shm_name` and no journal (2026-11-05, same Docker setup, 8 workers): anonymous `/` 12,938 req/s, logged-in front page 1,467,
node page 1,283, 0 statements per anonymous and 7 per logged-in request, first logged-in request after an fpm restart 155 ms.

The fast layer (`ChainedFastBackend` with nocopy instead of APCu, here with `routes` added to `bootstrap`, `config`, `discovery`) is the setup
that also works on several web servers. It stays close to stock, with or without `routes` (806–1,001 req/s logged-in node page without):
chainedfast still asks the database for each bin's last write on every request, and those bins were never the bottleneck. The gain
comes from moving all bins and the cache tags off the database, which needs one cache for every process of the site: `nocopy.shm_name`
on a single server, `nocopy.valkey` on several ([details](drupal)).

## Features

- **APCu-compatible.** `apcu_*`, `APCUIterator`, the `APC_*` constants and the `apc.*` INI names. Passes APCu's own test suite except three tests for documented non-goals ([differences](#differences-from-apcu)).
- **Zero-copy reads** under php-fpm, CGI, Apache, LiteSpeed, `php -S` and FrankenPHP classic mode. Long-running workers and the CLI get a safe **copy mode** that still reuses private copies of unchanged entries.
- **Lock-free reads, sharded writes.** Up to 8 shards with their own locks. A writer killed with `kill -9` mid-write is detected and its shard repaired.
- **Smarter eviction when full.** APCu wipes the whole cache. nocopy evicts its oldest segments and keeps entries read since they were written, using Segcache-style merging with a ghost list for keys that return.
- **Stampede protection.** `nocopy_remember()` and `apcu_entry()` run the callback once per key across all workers. `nocopy_remember()` also refreshes early via XFetch.
- **Symfony bundle** with `cache.adapter.nocopy`, which stores values without Symfony's marshaller so fetches stay zero-copy.
- **Valkey tier** (`nocopy.valkey`): Valkey becomes the cache of record shared by all servers, nocopy the zero-copy local copy in front of it. nocopy.so speaks RESP3 itself (no phpredis) and keeps local copies in sync through Valkey's client-side tracking.
- **Drupal module** with a cache backend for all bins and cache tag checksums, shared by php-fpm and Drush (`nocopy.shm_name`, or `nocopy.valkey` for several servers).

## Quick start

Requires PHP 8.3+ with `phpize` (e.g. the `php-dev` package), a C compiler, and Linux (CI-tested on amd64 and arm64).

```sh
git clone <this repository> nocopy && cd nocopy
phpize && ./configure && make
php -d extension=$PWD/modules/nocopy.so -d apc.enable_cli=1 \
    -r 'apcu_store("a", [1, 2, 3]); var_dump(apcu_fetch("a"));'
```

To enable it for your server, load it in `php.ini` (or a `conf.d` file) **instead of** APCu:

```ini
extension=/path/to/nocopy/modules/nocopy.so
nocopy.shm_size=128M
```

Existing `apcu_*` code and `apc.shm_size` / `apc.ttl` / `apc.entries_hint` settings keep working.

## Usage

### Native API

```php
nocopy_put('config', $config, ttl: 300);         // throws on objects unless $serialize: true
$config = nocopy_get('config', $hit);            // $hit tells a miss from a stored null/false
nocopy_delete('config');

// Batches: with nocopy.valkey, all local misses in one MGET and all writes in one pipeline
$found = nocopy_get_multiple(['a', 'b']);       // hits only, as key => value
nocopy_put_multiple(['a' => 1, 'b' => 2], ttl: 60);

// Compute once across all workers, refresh shortly before expiry
$report = nocopy_remember('report', fn (string $key) => buildReport(), ttl: 60);

print_r(nocopy_stats());                         // hits, misses, evicted, expired, expunges, used, …
echo nocopy_shared_with();                       // who sees these writes: process, host, valkey or none
```

`nocopy_remember($key, $callback, $ttl = 0, $beta = 1.0)`: on a miss, the first worker takes a lease and runs the callback.
The others wait for its value. A dead lease holder is detected, a stuck one is ignored after 30 s, and a recursive call on the same key does not wait.
Before the TTL ends, one worker refreshes early ([XFetch](https://doi.org/10.14778/2757807.2757813), Vattani et al., VLDB 2015) with a probability that rises
as expiry nears, scaled by the callback's measured run time × `$beta`. The others keep getting the current value. `$beta = 0` turns this off.

### Symfony

Symfony's built-in `cache.adapter.apcu` serializes values before they reach the extension, which removes the zero-copy benefit.
The bundle in [`symfony/`](symfony) (`chrfritsch/nocopy-bundle`, Symfony 6.4 / 7.x / 8.x) adds a pool that stores values unserialized.
Until it is on Packagist, install it from a checkout:

```sh
composer config repositories.nocopy path /path/to/nocopy/symfony
composer require chrfritsch/nocopy-bundle:@dev
```

Register `Chrfritsch\NocopyBundle\NocopyBundle` in `config/bundles.php`, then:

```yaml
# config/packages/cache.yaml
framework:
    cache:
        app: cache.adapter.nocopy
```

The adapter uses `nocopy_*` only, so it also works with `nocopy.apcu_compat=0` and next to real APCu.
`clear()` bumps a namespace version instead of deleting keys, and old entries leave through normal eviction.

### Drupal

The module in [`drupal/`](drupal) (`chrfritsch/nocopy-drupal`, Drupal 11.2+) puts every cache bin and the cache tag checksums
in nocopy, so cache reads and tag checks no longer touch the database. Install it, enable it, and add the `settings.php` lines from
[`drupal/README.md`](drupal/README.md). It needs a cache shared by every PHP process of the site: `nocopy.shm_name` on a single
server (Drush and cron attach to php-fpm's memory, so `drush cr`, `drush cim` and `drush cron` reach php-fpm directly), `nocopy.valkey`
on several. A process without one throws instead of serving stale data.

Reproduce the benchmark:

```sh
docker build --build-arg PHP=8.5 -t nocopy-linux:8.5 docker && docker build -t nocopy-drupal -f docker/Dockerfile.drupal docker
docker run --rm --shm-size=1g -v "$PWD":/src:ro -v nocopy-drupal-bench:/site -v "$PWD/bench/results-drupal":/out nocopy-drupal /src/docker/drupal-bench.sh
```

## Configuration

All settings are `PHP_INI_SYSTEM` (php.ini only).

| Setting | Default | Meaning |
|---|---|---|
| `nocopy.shm_size` | `32M` | Arena size. Alias: `apc.shm_size` |
| `nocopy.shm_name` | (off) | Name of a POSIX shared-memory object, e.g. `/nocopy`: the CLI (Drush, cron), other fpm pools and restarted servers attach to the same cache ([below](#shared-by-name)). A process that cannot attach has no cache. Off: shared by php-fpm's or the server's own workers only, gone when it stops |
| `nocopy.ttl` | `0` | Default TTL in seconds (0 = none). Alias: `apc.ttl` |
| `nocopy.entries_hint` | `4096` | Sizes the fixed slot table (~2 slots per hinted entry, at least one per 256 B of arena). Raise it for many entries under ~300 B, or the cache evicts before its memory is full. Alias: `apc.entries_hint` |
| `nocopy.request_scope` | `auto` | `auto`: zero-copy under fpm, CGI, Apache, LiteSpeed, `php -S` and FrankenPHP classic mode, copy mode elsewhere. `1`: zero-copy also under the CLI (pins every segment read until the script ends). `0`: always copy |
| `nocopy.local_size` | `8M` | Copy mode only: keeps private copies of arrays and strings (≥ 512 B) fetched twice and hands them out again while the entry is unchanged. Counts against `memory_limit`. `0` = off |
| `nocopy.serializer` | `php` | `php` or `igbinary`: the format for values stored serialized (objects, shared references). Arrays and scalars are never serialized. `igbinary` needs the igbinary extension, otherwise a startup warning and `php`. Alias: `apc.serializer` |
| `nocopy.apcu_compat` | `1` | Registers `apcu_*`, `APCUIterator`, `APC_*` and `apc.*`. `0`: only `nocopy_*`, so real APCu can be loaded too. If APCu is already loaded, nocopy switches to `0` itself and logs a startup warning |
| `nocopy.valkey` | (off) | `tcp://[user:pass@]host:port`, `tls://…` or `rediss://…` (TLS), `host:port` or `unix:///path`: share the cache through Valkey ([below](#valkey)). `phpinfo()` hides the credentials |
| `nocopy.valkey_prefix` | `nocopy:` | Key prefix in Valkey; servers with the same prefix share one cache |
| `nocopy.valkey_timeout` | `0.5` | Seconds to wait for a connect or a reply before treating Valkey as down |
| `nocopy.valkey_tls_ca_file` | (system store) | PEM file of the CAs that may sign the server certificate, for `tls://` / `rediss://` |
| `nocopy.valkey_tls_cert_file`, `nocopy.valkey_tls_key_file` | (none) | Client certificate (chain) and key, where the server asks for one (`tls-auth-clients`) |
| `nocopy.valkey_tls_verify` | `1` | Verify the server certificate and its name (or IP); `0` only for tests |
| `apc.enabled`, `apc.enable_cli` | `1`, `0` | As in APCu. These switch the `apcu_*` functions, not `nocopy_*` |

If both an `apc.*` alias and its `nocopy.*` name are set, the `nocopy.*` value wins and a startup warning is logged.
FrankenPHP worker scripts switch to copy mode at their first `frankenphp_handle_request()`, whatever `request_scope` says.

### Shared by name

By default the cache is created when the server starts and shared only by its own workers: the CLI gets a private one,
and a restart or `reload` starts cold. With `nocopy.shm_name=/nocopy` the first process creates `/dev/shm/nocopy.<build id>`
and every later one attaches to it at the same address, so a CLI script and php-fpm (or FrankenPHP) share one cache, both ways, and it survives restarts.
Readers attached this way still get zero-copy values (fpm, FrankenPHP classic mode); the CLI uses copy mode as before.

- **All or nothing.** A process that cannot attach (another user's, `/dev/shm` full) logs a startup warning and has no cache: fetches miss,
  writes return `false`, as with Valkey down. Never a private cache instead: its writes and deletes would not reach the others.
  Errors in the values themselves (an object without `$serialize`) still throw.
- **One user.** The object is created with mode `0600`, so every process must run as the user that created it: the one that starts php-fpm
  (often root; its workers inherit the mapping), and Drush, cron and workers as that user too. Anyone who can write it controls what fpm reads: do not widen the mode.
- **One build.** The build id is a hash of the loaded `nocopy.so`, so php-fpm and the CLI share only when they load the same file.
  A process that finds another build's arena under the same name still in use warns that those processes do not share this cache; unused ones
  (left by an upgrade) are removed. Restart long-running processes after an upgrade.
- **Memory is reserved up front.** The creator reserves the whole `nocopy.shm_size` in `/dev/shm`, so it counts as shared memory from the start
  and can never run out under a reader. In Docker, raise `--shm-size` (64M by default). Later processes get the creator's size.
- One PID namespace: a dead lock holder is detected by its pid, so not across containers.
- `nocopy_shared_with()` tells a process where its writes go: `process` (its own memory and its forks'), `host` (this name), `valkey`, or `none`.
  `nocopy_stats()['shm_name']` is the object's name. The CLI attaches whatever `apc.enable_cli` says (that only switches the `apcu_*` functions).
- Linux (CI: `docker/shm-name.sh` with php-fpm and FrankenPHP); macOS untested.

### Valkey

With `nocopy.valkey` set, every write goes to Valkey and to the local arena, every local miss is fetched from Valkey and kept locally,
and deletes, `apcu_inc/dec/cas` and `apcu_clear_cache` run in Valkey. `nocopy_get_multiple()` and `apcu_fetch(array)` fetch all local misses
in one `MGET`; `nocopy_put_multiple()` and `apcu_store(array)` send their `SET`s in one pipeline. Each worker has a plain command connection;
one listener process per local cache (`nocopy-valkey` in `ps`) holds the only tracking connection (`CLIENT TRACKING ON BCAST PREFIX <prefix>`):
a change made anywhere, including expiry and eviction in Valkey, drops the local copy as soon as it arrives, once per server instead of once
per worker. Local hits stay zero-copy and cost no round trip. So several servers share one cache, a php-fpm restart
or a new server starts warm, and CLI processes (Drush, cron, workers) see web writes and the other way round.

Drupal 11, 2,000 nodes, php-fpm 8 workers, all bins and tags shared (one run, `bench/results-drupal/valkey`). The baseline is the
usual shared setup: drupal/redis with phpredis on the same Valkey, APCu chainedfast in front of bootstrap/config/discovery.

| req/s (p50) | drupal/redis | nocopy + Valkey | nocopy, single server |
|---|---|---|---|
| anonymous `/` | 5,129 (2 ms) | 12,546 (1 ms) | 9,109 (1 ms) |
| logged-in `/` | 727 (10 ms) | 1,437 (5 ms) | 1,454 (5 ms) |
| logged-in node | 711 (11 ms) | 1,299 (6 ms) | 1,239 (6 ms) |
| first logged-in request after an fpm restart | 157 ms | 182 ms | 504 ms |
| second server, requests 6-30 (p50) | 6.6 ms | 4.3 ms | n/a |

A web save on server A and a Drush save both show up on server B in both shared setups. Anonymous traffic beats the single-server
mode of that run because Valkey replaced the journal query that every request paid then (the journal is gone since).

- The listener is started by the first process that needs it (and again by the next one if it dies: no heartbeat for 1 s). It pings
  Valkey every second; when its connection is lost or silent, nothing local is served until it is back, and it clears the local cache on
  every (re)connect, since it cannot know what it missed. With `nocopy.shm_name` it outlives the servers: the local cache stays in sync and
  warm across php-fpm or FrankenPHP restarts, and it exits when the segment is removed (`rm /dev/shm/<name>.*`). Without, it exits with the
  last process of the server.
- Valkey unreachable: nocopy serves no local data (it could be stale), fetches miss and writes return `false`, with one warning per attempt,
  retried every 0.5-1.5 s (jittered) per worker. A connection found closed at the start of a request is reopened at once. Each connection
  subscribes to one idle channel, so Valkey's `timeout` does not close it, and uses TCP keepalive (60 s) against load balancers that drop idle
  connections. In `CLIENT LIST` the listener is `name=nocopy-listener`, workers `name=nocopy`.
- Works on any Valkey (or Redis 6.2+) with RESP3, managed services included. TLS: `nocopy.valkey=tls://user:pass@host:6380` (or `rediss://`),
  TLS 1.2+, the server certificate verified against the system store or `nocopy.valkey_tls_ca_file`, with SNI and the host name (or IP)
  checked; client certificates for mutual TLS. The files are read once at startup (by php-fpm's master, so the key may be readable by root
  only); an unreadable one is a startup warning and every connection fails with it. Built when OpenSSL (1.1.1+, headers and pkg-config)
  is found; a build without it says so on connect. An optional nocopy module for Valkey is planned to save round trips.
- Local only: `APCUIterator`, `apcu_cache_info`, `nocopy_stats()`, and the single flight of `nocopy_remember`/`apcu_entry` (per server).
- Valkey evicts by its own `maxmemory-policy`; an evicted key is a miss everywhere. The Drupal module is built for `volatile-lfu` ([details](drupal/README.md)).
- Values are stored with `nocopy.serializer` (integers as plain numbers), so all servers need the same serializer.
- Every change is pushed to every server's listener, the writer's own server included (its local copy is refetched once): meant for cache
  traffic, not for counters hammered on every request.

## How it works

```mermaid
flowchart LR
    W1[php-fpm worker] -- "fetch: pointer, no lock" --> A
    W2[php-fpm worker] -- "store: shard lock" --> A
    subgraph A["mmap arena (shared by all workers)"]
        S1["shard 1: slot table + segments"]
        S2["shard 2 … 8"]
    end
```

- **Arena:** one `mmap(MAP_SHARED|MAP_ANONYMOUS)` created at startup, so every forked worker sees it at the same address.
- **Segments:** writes append to 64 KB–2 MB segments, bucketed by TTL. Expired entries are swept a few at a time on each write.
- **Pins:** each worker marks the segments it holds values from. A segment is reused only when no worker pins it, so zero-copy values stay valid until the request ends.
- **Merge:** when a shard is full, sparse segments are compacted. Failing that, the oldest are merged: recently read entries move forward and the rest are evicted, a few hundred at a time.

The full design, including invariants and failure handling, is in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Differences from APCu

The API behaves the same. What depends on APCu's internal layout differs by design:

| | APCu | nocopy |
|---|---|---|
| Cache full | wipes everything | evicts oldest segments, keeps recently read entries |
| `apcu_entry()` | callback runs under the global cache lock | single-flight per key, other keys stay writable |
| `apcu_sma_info()` | one segment | one "segment" per shard (an entry fits only in its own shard) |
| Per-entry `num_hits`, `access_time`, `ref_count` | counted | `0`, creation time, `0` (no shared write per read) |
| `APCUIterator`, `cache_list` | under the cache lock | lock-free: under concurrent writes an entry may be skipped or seen twice |
| `apc.serializer` | used for every value | used only for objects and shared references; arrays/scalars are stored natively |
| `apc.preload_path`, `apc.slam_defense`, `apc.gc_ttl`, `apc.mmap_file_mask` | supported | ignored |

Other limits: the arena is split into up to 8 shards of at least 8 MB each, and a single entry must fit into one shard.
Under ZTS, nocopy is tested with `parallel` and FrankenPHP, not with mod_php under a threaded MPM.

## Development

```sh
phpize && ./configure && make
make test TESTS=tests                                            # .phpt suite (a debug PHP build catches more)
PHP=php NOCOPY_SO=$PWD/modules/nocopy.so scripts/apcu-suite.sh   # APCu's own suite against nocopy
tests/c/run.sh                                                   # arena alone, 8 threads, plain + ThreadSanitizer
```

The Linux toolchain used by CI lives in [`docker/`](docker):

```sh
docker build --build-arg PHP=8.5 -t nocopy-linux docker
docker run --rm -v "$PWD":/src:ro nocopy-linux /src/docker/test.sh      # build + test suite
docker run --rm -v "$PWD":/src:ro nocopy-linux /src/docker/stress.sh 30 # race stress loop
```

Benchmarks against APCu need release builds of PHP and both extensions: `PHP=… NOCOPY_SO=… APCU_SO=… bench/all.sh`.
CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) covers PHP 8.3/8.4/8.5 on amd64 and arm64, ZTS 8.4, FrankenPHP and Symfony 6.4/7.4/8.x.

For project state, measurement history and next steps, see [`HANDOVER.md`](HANDOVER.md).

## Contributing

Bug reports and pull requests are welcome. Please include the PHP version, SAPI (fpm, FrankenPHP, CLI…), NTS/ZTS and the output of `nocopy_stats()`.
Changes to locking or eviction should pass the stress loops (`docker/stress.sh`, `tests/c/run.sh`). See the invariants in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## License

[MIT](LICENSE) © 2026 Christian Fritsch.
Eviction design inspired by [Segcache](https://www.usenix.org/conference/nsdi21/presentation/yang-juncheng) (Yang et al., NSDI 2021); API compatible with [APCu](https://github.com/krakjoe/apcu).
