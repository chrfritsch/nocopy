# stash

New here? Read `HANDOVER.md` (state, rebuild steps, results, next steps), then `ARCHITECTURE.md`.

A zero-copy, shared-memory cache for PHP with an `apcu_*` compatibility layer.

Values live in one `mmap` arena shared by all forked workers. Arrays and strings are stored as
opcache-style immutable structures, so `apcu_fetch()` returns a pointer into shared memory instead
of copying or unserializing. Readers take no locks. The arena is a log of segments (64 KB-2 MB): writes append,
garbage is compacted forward, and a segment is reused once no worker pins it.

## Build

```sh
phpize && ./configure && make
php -d extension=modules/stash.so -d apc.enable_cli=1 -r 'apcu_store("a", [1,2,3]); var_dump(apcu_fetch("a"));'
```

Tested on PHP 8.3, 8.4, 8.5 (Linux amd64/arm64) and 8.7-dev (macOS), NTS only. Linux in Docker: `docker/` (`test.sh`, `stress.sh`, `bench.sh`, `fpm-bench.sh`).
Cannot be loaded together with real APCu (same function names).

## Test

```sh
make test TESTS=tests                          # own suite (debug PHP build recommended: poisons freed memory)
PHP=... STASH_SO=$PWD/modules/stash.so scripts/apcu-suite.sh   # APCu's suite; expect 59/61
```

## Benchmark (use a release PHP and release-built extensions)

```sh
PHP=... STASH_SO=... APCU_SO=... bench/all.sh
```

## Modes

| INI | meaning |
|---|---|
| `apc.enabled`, `apc.enable_cli`, `apc.shm_size`, `apc.entries_hint`, `apc.ttl` | as in APCu |
| `stash.request_scope=auto` (default) | zero-copy for the whole request under fpm/cgi/apache/litespeed; worker mode (below) under CLI and others |
| `stash.request_scope=1` | whole request may hold zero-copy values; pinned lazily on first handout. Only the segments actually handed out stay pinned until the request ends (or `stash_checkpoint()`) |
| `stash.request_scope=0` | worker mode: fetch returns a copy unless inside `stash_scope(fn)` |

Extra functions: `stash_put/get/delete`, `stash_scope`, `stash_checkpoint`, `stash_stats`.

## Symfony

Symfony's default `cache.adapter.apcu` serializes values before they reach the extension, which hides the benefit. Use a pool without a marshaller
(`new ApcuAdapter($ns, 0, null, null)`, see `bench/sfapp`): about 3.5x requests/s on cache-heavy endpoints vs APCu with the same pool, 4.8x vs Symfony's default APCu pool (Linux, php-fpm; details in `HANDOVER.md`).

## When the cache is full

Unlike APCu, which wipes the whole cache, stash evicts its oldest segments (FIFO, a few hundred entries at a time); `stash_stats()['expunges']` counts them.

## Known gaps

No `preload_path`, no slam defense, no per-entry hit counts (would add a shared write per read),
`apcu_entry` is not single-flight,
no thread-safe (ZTS) builds yet (FrankenPHP, mod_php with a threaded MPM): the build refuses them.
Values stored by side effect inside `stash_scope` dangle after the scope.
