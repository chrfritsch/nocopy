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
Loads next to real APCu only with `stash.apcu_compat=0` (stash switches to it by itself, with a startup warning, when APCu is loaded): configure it with `stash.*` and the same php.ini works either way.

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
| `stash.shm_size` (32M), `stash.ttl` (0) | as APCu's `apc.shm_size`, `apc.ttl` |
| `stash.entries_hint` (4096) | sizes the slot table, which never grows: ~2 slots per hinted entry, at least one per 256 bytes of `shm_size`. Raise it for many entries under ~300 bytes, or the cache evicts before its memory is full |
| `apc.shm_size`, `apc.ttl`, `apc.entries_hint` | aliases of the `stash.*` names (APCu layer only): used when set and the `stash.*` name is not; both set: `stash.*` wins, with a startup warning. `ini_get()` of either shows the value in effect |
| `apc.enabled`, `apc.enable_cli` | as in APCu; they switch the `apcu_*` functions, not `stash_*` |
| `stash.apcu_compat=1` (default) | `apcu_*`, `APCUIterator`, `APC_*` constants and the `apc.*` INI. `0`: only `stash_*`; real APCu can load too |
| `stash.request_scope=auto` (default) | zero-copy for the whole request under fpm/cgi/apache/litespeed/FrankenPHP (classic); worker mode (below) under CLI, FrankenPHP worker scripts and others |
| `stash.request_scope=1` | zero-copy for the whole request, also under CLI. The segments handed out stay pinned until the request ends: a long-running script holds every segment it read, so use `0` there |
| `stash.request_scope=0` | worker mode: fetch always returns a private copy |
| FrankenPHP worker | copy mode from its first `frankenphp_handle_request()` on, whatever `stash.request_scope` says |

A fetched value can never outlive the memory it points to: zero-copy is offered only where the request ends.

Extra functions: `stash_put/get/delete`, `stash_remember`, `stash_stats`. `stash_put()` throws on objects unless its 4th argument `$serialize` is true.

`stash_remember($key, $callback, $ttl = 0, $beta = 1.0)` fetches, or runs `$callback($key)` and stores its result (objects serialized).
Concurrent misses run the callback once: the first worker takes a lease, the others wait for its value (a dead holder is detected, a stuck one is ignored after 30 s, a recursive call on the same key does not wait).
Before the TTL ends it refreshes early by XFetch (Vattani et al., VLDB 2015): with probability rising as expiry nears, scaled by the callback's measured run time × `$beta`; one worker refreshes, the others keep getting the current value. `$beta = 0` turns that off.
`apcu_entry` is single-flight the same way (per key, not the whole cache like APCu), without early refresh.

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

Unlike APCu, which wipes the whole cache, stash merges its oldest segments: entries read since they were written survive, the rest is evicted (a few hundred at a time); `stash_stats()['expunges']` counts the rounds. `evicted` / `expired` count the entries dropped by merge / by their TTL; with `hits` / `misses` they show whether `apc.shm_size` is too small. A key written again soon after it was evicted starts as if read (`ghost_hits`), so keys that recur just beyond the cache size are kept over keys used once.
The arena is split into up to 8 shards of at least 8 MB each (1 shard up to 8 MB, 4 at the default 32 MB); one entry must fit in a shard, larger ones are refused.

## APCu: same API, not the same layout

`apcu_*` calls behave like APCu's (APCu's own .phpt suite: all but `apc54_014`, `preload_path`, and `apcu_sma_info`, see below).
What reports or depends on APCu's memory layout and bookkeeping differs, by design:

| | APCu | stash |
|---|---|---|
| cache full | wipes everything | merges its oldest segments, keeps what was read since (`expunges` counts the rounds) |
| `apcu_sma_info()` | one segment, its free blocks | one "segment" per shard (`num_seg` = shards, `seg_size` = slice), free runs per slice: an entry only fits into its own shard |
| `num_hits` (per entry), `access_time`, `ref_count` | counted | 0, creation time, 0: a shared write per read would cost more than the counters are worth |
| `APCUIterator`, `apcu_cache_info()['cache_list']` | under the cache lock | lock-free: under concurrent writes an entry may be skipped or seen twice |
| `apc.serializer` | used for every value | ignored: arrays and scalars are stored as PHP structures, objects with PHP's `serialize()` |
| `apc.preload_path`, `apc.slam_defense`, `apc.gc_ttl`, `apc.mmap_file_mask` | supported | ignored |
| `apcu_entry()` | runs the callback under the cache lock | single-flight per key: other keys stay writable meanwhile |

## Known gaps

ZTS is tested with `parallel` and FrankenPHP only (not mod_php with a threaded MPM).
