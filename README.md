# stash

New here? Read `HANDOVER.md` (state, rebuild steps, results, next steps), then `ARCHITECTURE.md`.

A zero-copy, shared-memory cache for PHP with an `apcu_*` compatibility layer.

Values live in one `mmap` arena shared by all forked workers (or threads, ZTS). Arrays and strings are stored as
opcache-style immutable structures, so `apcu_fetch()` returns a pointer into shared memory instead
of copying or unserializing. Readers take no locks. The arena is a log of segments (64 KB-2 MB) split into fixed per-shard slices: writes append,
garbage is compacted forward, a full cache evicts its oldest entries but keeps the ones read since (Segcache-style merge), and a segment is reused once no worker pins it.

## Build

```sh
phpize && ./configure && make
php -d extension=modules/stash.so -d apc.enable_cli=1 -r 'apcu_store("a", [1,2,3]); var_dump(apcu_fetch("a"));'
```

Tested on PHP 8.3, 8.4, 8.5 (Linux arm64, CI also amd64), NTS and ZTS (8.4, incl. FrankenPHP classic and worker mode). Linux in Docker: `docker/` (`test.sh`, `stress.sh`, `bench.sh`, `ab.sh`, `fpm-bench.sh`, `frankenphp.sh`, `Dockerfile.zts`).
Loads next to real APCu only with `stash.apcu_compat=0` (stash switches to it by itself, with a startup warning, when APCu is loaded).

## Test

```sh
make test TESTS=tests                          # own suite (debug PHP build recommended: poisons freed memory)
PHP=... STASH_SO=$PWD/modules/stash.so scripts/apcu-suite.sh   # APCu's suite; expect 59/61
tests/c/run.sh                                 # the arena alone, from 8 threads, plain and ThreadSanitizer (Linux)
```

## Benchmark (use a release PHP and release-built extensions)

```sh
PHP=... STASH_SO=... APCU_SO=... bench/all.sh
```

## Modes

| INI | meaning |
|---|---|
| `apc.enabled`, `apc.enable_cli`, `apc.shm_size`, `apc.ttl` | as in APCu |
| `stash.apcu_compat=1` (default) | `apcu_*`, `APCUIterator`, `APC_*` constants and `apc.*` INI. `0`: only `stash_*`, sized by `stash.shm_size`, `stash.entries_hint`, `stash.ttl`; real APCu can load too |
| `apc.entries_hint` | sizes the slot table, which never grows: ~2 slots per hinted entry, at least one per 256 bytes of `shm_size`. Raise it for many entries under ~300 bytes, or the cache evicts before its memory is full |
| `stash.request_scope=auto` (default) | zero-copy for the whole request under fpm/cgi/apache/litespeed/FrankenPHP (classic); worker mode (below) under CLI, FrankenPHP worker scripts and others |
| `stash.request_scope=1` | whole request may hold zero-copy values; pinned lazily on first handout. Only the segments actually handed out stay pinned until the request ends (or `stash_checkpoint()`) |
| `stash.request_scope=0` | worker mode: fetch returns a copy unless inside `stash_scope(fn)` |
| FrankenPHP worker + `stash.request_scope=1` | zero-copy, and each `frankenphp_handle_request()` ends like `stash_checkpoint()`: values kept past a request dangle |

Extra functions: `stash_put/get/delete`, `stash_scope`, `stash_checkpoint`, `stash_stats`. `stash_put()` throws on objects unless its 4th argument `$serialize` is true.

## Symfony

Symfony 8's `cache.adapter.apcu` (not 6.4's) serializes values before they reach the extension, which hides the benefit. The bundle in `symfony/` (`Lab\StashBundle\StashBundle`) adds `cache.adapter.stash`: `StashAdapter` on `stash_*`, no marshaller, so it also works with `stash.apcu_compat=0` and next to real APCu:

```yaml
framework:
    cache:
        app: cache.adapter.stash
```

About 3.5x requests/s on cache-heavy endpoints vs APCu with the same pool, 4.8x vs Symfony's default APCu pool (Linux, php-fpm; details in `HANDOVER.md`).
`clear()` bumps a namespace version (no prefix delete in `stash_*`): old entries stay until eviction drops them, unread ones first. The pool empties on each new container build, like APCu's.
`cache.system` already stores values unserialized when the APCu layer is on. Check: `php -d extension=stash.so -d zend.assertions=1 symfony/tests/smoke.php <dir with vendor/>`.

## When the cache is full

Unlike APCu, which wipes the whole cache, stash merges its oldest segments: entries read since they were written survive, the rest is evicted (a few hundred at a time); `stash_stats()['expunges']` counts the rounds.
The arena is split into up to 8 shards of at least 8 MB each (1 shard up to 8 MB, 4 at the default 32 MB); one entry must fit in a shard, larger ones are refused.

## Known gaps

No `preload_path`, no slam defense, no per-entry hit counts (would add a shared write per read),
`apcu_entry` is not single-flight,
ZTS is tested with `parallel` and FrankenPHP only (not mod_php with a threaded MPM).
Values stored by side effect inside `stash_scope` dangle after the scope.
