# Handover: stash

Read this first. `README.md` = user-facing usage. `ARCHITECTURE.md` = how the arena works.
This file = state, how to rebuild everything, what was measured, what to do next.

## Goal

A sellable PHP extension in the style of Relay: a zero-copy shared-memory cache that is a drop-in
for APCu (`apcu_*`, `APCUIterator`, `apc.*` INI) and adds extras (`stash_*`). Chosen after reviewing
php-src for weak spots (opcache-style immutable arrays make zero-copy reads possible).

## State (2026-11-03)

Works, not released. Not a git repo yet, no license, no CI, only tried on PHP 8.7-dev (master), macOS arm64, NTS.
- Own tests: `tests/001..018` pass (release and debug builds; 012 needs a PHP with `session`). 015 = 8-process stress: loop it on a debug build after locking changes.
- TTL-bucketed segments with incremental expiry (2026-11-03, see ARCHITECTURE.md); `stash_stats()` has `evacuated_bytes` and `shards`.
- Sharded writes (2026-11-03): per-shard locks, a pool lock for segments; see ARCHITECTURE.md "Shards".
- Growable slot tables (2026-11-03): a shard's table doubles into pool segments when it fills; `stash_stats()` has `slots`. 018 loops clean 60x on debug.
- Review fixes (2026-11-03): arena strings no longer `IS_STR_PERMANENT` (PCRE kept them as process-wide cache keys);
  pins dropped in post-deactivate, not RSHUTDOWN (session's RSHUTDOWN serialized `$_SESSION` after ours: test 012 segfaulted);
  `apcu_cache_info`/`APCUIterator`/`apcu_sma_info` no longer allocate under the lock (OOM inside it deadlocked every worker: test 013); ZTS builds refused with `#error`.
- APCu's own .phpt suite: 59 pass / 2 fail. Failures are non-goals: `apc54_014` (preload_path), `ghbug335` (slam defense).
- Symfony Cache: `ApcuAdapterTest` 154 tests / 285 assertions / 6 skipped (same as real APCu); full Cache suite
  was identical to real APCu before the segment rewrite (6841 tests / 5403 assertions / 4066 skipped) and has NOT been re-run since.

## Rebuild the toolchain (nothing but the source lives in this directory)

1. brew: `autoconf automake libtool re2c bison pkgconf`; put `/opt/homebrew/opt/bison/bin` on PATH.
2. In php-src (`~/Sites/php-src`): `./buildconf`, then configure into a throwaway prefix (never into the repo):
   - debug, small: `--disable-all --enable-cli --enable-pcntl --enable-tokenizer --enable-debug` (poisons freed memory; use for tests)
   - release for benchmarks + Symfony: same without `--enable-debug`, plus the extensions Symfony needs
     (ctype dom fileinfo filter mbstring openssl posix session simplexml xml xmlwriter zlib opcache; check against `php -m` of the last build:
     Core ctype date dom fileinfo filter hash json lexbor libxml mbstring openssl pcntl pcre Phar posix random Reflection session SimpleXML SPL standard tokenizer uri xml xmlwriter Zend OPcache zlib).
3. Build stash out of tree (copy sources to a build dir, then): `phpize && ./configure --with-php-config=<prefix>/bin/php-config && make`.
   After restoring/replacing `stash.c`, force a rebuild (`rm -f stash.lo .libs/stash.o modules/stash.so`): stale builds fooled us twice.
4. APCu for comparison: `git clone https://github.com/krakjoe/apcu`, build the same way. stash and APCu cannot be loaded together.
5. Symfony Cache tests: clone `symfony/cache` (split repo), `composer install` (use `--ignore-platform-req=php`), run `vendor/bin/phpunit Tests/Adapter/ApcuAdapterTest.php` with `-d extension=stash.so -d apc.enable_cli=1`.
6. Real app benchmark: `bench/sfapp/` holds only `src/`, `public/`, `composer.json`. Copy to a work dir, `composer install --ignore-platform-req=php`, `composer dump-autoload -o`.

## Run things

| What | Command |
|---|---|
| own tests | `TEST_PHP_EXECUTABLE=<php> <php> run-tests.php -n -d extension=$PWD/modules/stash.so tests` |
| APCu suite | `PHP=<php> STASH_SO=<so> scripts/apcu-suite.sh [workdir]` (expect 59/61) |
| micro benchmarks vs APCu | `PHP=.. STASH_SO=.. APCU_SO=.. bench/all.sh` (release builds only) |
| Symfony adapter benchmarks | `PHP=.. STASH_SO=.. APCU_SO=.. SF_AUTOLOAD=<symfony/cache>/vendor/autoload.php bench/symfony-all.sh` |
| real Symfony app (php -S, 8 workers, ab) | `PHP=.. STASH_SO=.. APCU_SO=.. APP=<app dir> bench/sfapp.sh` |

Gotchas: zsh scripts (`${=VAR}` for word splitting); `-n` to ignore php.ini; the benchmark scripts guard against a
misconfigured APCu `shm_size`; in macOS `sed -i ''`; run-tests.php is the one installed with PHP, not APCu's.

## Measured results (macOS, release -O2, single machine: treat as indicative; re-run on Linux)

Micro (`bench/all.sh`): scalar fetch 2.3x APCu; zero-copy array fetch ~15 ns regardless of size (10k rows: 2.5 ms APCu php-serializer,
1.0 ms APCu default serializer); copy mode (worker-safe) array fetch only ~0.8x of APCu default serializer; 8 workers parallel scalar fetch ~50x
(APCu throughput falls with workers; part may be a macOS lock artifact); writes 1.0-1.5x, but `store string 100B` 0.7x (~95 ns vs 58).

Contention (`run.php contend`, copy mode, median of 3, M ops/s) before -> after the lock/cache-line changes. Ablation = after, with one change reverted:

| | APCu | before | after | w/o local slot mask | w/o TTAS+yield lock | w/o one-lock small writes |
|---|---|---|---|---|---|---|
| store int, 4 workers | 0.26 | 2.16 | 9.88 | 8.68 | 3.88 | 3.03 |
| store int, 8 workers | 0.26 | 0.70 | 6.02 | 4.17 | 0.98 | 1.88 |
| store int, 16 workers (oversubscribed) | 0.26 | 0.47 | 9.35 | 6.81 | 0.80 | 2.72 |
| 4 readers fetch int, 4 writers running | 0.14 | 38 | 162 | 70 | 94 | 137 |
| 4 readers fetch int, no writers | 7.7 | 200 | 199 | 199 | 198 | 200 |

Single-process numbers did not move (store int 36 ns, store string 100B 83 -> 78 ns vs APCu 57, all fetches within noise):
the remaining small-write gap is not the lock, look at `calc_zval`/`time()`/`locate` next.

TTL buckets (`run.php ttl`: 50k long-lived 1 KB values rewritten at random + a stream of unique ttl=1 values, 512M arena, 5 s, median of 3):

| | before | after | APCu |
|---|---|---|---|
| failed stores | ~1.1M of ~9M | 0 | 0 |
| slowest store | 33-37 ms | 0.2-1.8 ms | 105-130 ms |
| stores > 50 us | ~3400 | ~2300 | ~20-140 |
| M stores/s | 1.78 | 1.87 | 2.0 |

Before, expired entries were swept only when an allocation failed (a full slot-table scan, at most once a second), so the arena filled with dead data and stores failed.
The ~2300 slow stores left are this benchmark's heavy pressure (evacuation, ~80 MB copied in 5 s); APCu has fewer but much longer stalls.
Regression from this change: `store int x16 workers` (oversubscribed, macOS) 9.3 -> 6.3 M ops/s; x4 and x8 are unchanged or better and I could not pin down why x16 moved
(not the per-write expiry step, not the segment metadata layout; moving `time()` back inside the lock recovered x4). Sharding (below) more than made up for it.

Shards (8 at 512M; `contend`/`write`/`ttl`/`read`, median of 3) before -> after:

| | before | after |
|---|---|---|
| store int, 4 workers (M ops/s) | 11.8 | 25.0 |
| store int, 8 workers | 5.6 | 15.4 |
| store int, 16 workers (oversubscribed) | 7.0 | 14.9 |
| 4 readers fetch int while 4 writers run | 161 | 89 |
| store int / inc / add, one process (ns) | 38 / 14.6 / 36.6 | 40.5 / 16.8 / 40.1 |
| ttl churn (M stores/s, one process under heavy pressure) | 1.83 | 1.64 |
| ttl churn slowest store | 1.9 ms | 1.0 ms |
| fetches (zero-copy) | | within noise |

The reader drop is CPU capacity, not stash: 4 plain PHP busy loops that never touch stash cut the same readers to 91-100 M ops/s on this 5P+6E Mac
(before, the writers mostly sat in the lock's yield). Single-process writes pay ~2-4 ns (shard pick, pool lock per new segment); ttl churn ~10% (cross-shard help under pressure).

Growable slot tables (same groups, median of 3, before -> after): everything within noise (store int 40.6 -> 42.2 ns, fetches -0.3 ns,
parallel stores +-4%, 4 readers under 4 writers 80 -> 74 M ops/s, a figure that has moved 80-95 between runs of one build).
The new cost is the grow stall: 3M unique puts at 512M grew 2M -> 4M slots, one ~2-2.8 ms put per shard.
018's readers are not sensitive to the table-pointer re-check in `find()` (removing it still passes 30/30): the window needs a key overwritten and its segment reused mid-probe.

Real Symfony 8.1 app, requests/s, median of 3 (`bench/sfapp.sh`):

| config | /hello | /products | /product/77 | /validate |
|---|---|---|---|---|
| APCu, Symfony default pool | 5403 | 2688 | 2698 | 4567 |
| stash, default pool | 5595 | 2743 | 2784 | 4404 |
| APCu, raw pool | 5720 | 3291 | 3338 | 4488 |
| stash copy, raw pool | 5793 | 3499 | 3446 | 4366 |
| **stash zero-copy, raw pool** | 5435 | **5622** | **5640** | 4655 |

KEY FINDING: Symfony's default `cache.adapter.apcu` wires `cache.default_marshaller`, so values are serialized to strings in PHP
before the extension sees them; stash then gives ~2%. The gain (~2.1x vs default, ~1.7x vs APCu raw) needs a pool without a marshaller
(`new ApcuAdapter($ns, 0, null, null)`, see `bench/sfapp/src/Kernel.php`). A Symfony bundle (`cache.adapter.stash`) that does this is the obvious product piece.
Earlier "zero-copy write is 50-100x slower" rows were a bug (retire-list pile-up), fixed by the segment rewrite; do not trust old result files.

## Key decisions (why things are the way they are)

- Zero-copy needs lifetime guarantees; they come from per-worker pin bitmaps over segments (see ARCHITECTURE.md). Replaced an epoch + per-entry retire list that piled up garbage.
- `stash.request_scope=auto`: zero-copy for the whole request only under fpm/cgi/apache/litespeed/cli-server; CLI and workers use copy mode (`stash_scope()` opts in per callback).
- Objects/shared references/arrays with refcount>1 references fall back to `serialize()`; resources are refused with APCu's warning.
- `serialize`/`unserialize` inside the extension need `BG(serialize_lock)++` (re-entrancy; APCu bug #76145).
- Per-entry hit counts are intentionally unsupported (would be a shared write per read).
- Comments in code: one short line only (user preference).

## Known gaps / next steps (rough priority)

1. Symfony bundle: `cache.adapter.stash` pool without marshaller; document the default-pool finding for customers.
2. Re-run on Linux + php-fpm; re-run the full Symfony Cache suite after the segment rewrite.
3. Robust process-shared locks on Linux (a writer dying inside a shard or the pool lock deadlocks everyone).
   Copy mode: dedupe array keys at persist time (pointer-keyed; PDO rows share key strings) and copy out in one pass.
   Shards: expiry of a shard without writes waits for memory pressure.
4. Compaction runs under the global lock (~ms stall); RSS drifts to `apc.shm_size` because segments are reused only under pressure; `MADV_HUGEPAGE`; faster `copy_out` (one pass).
5. Milestone 3 features that justify a price: tags, single-flight `apcu_entry`, cross-server invalidation. Bigger idea: Redis client-side cache tier (Relay model, RESP3 tracking), where the gain is 100x not 2x.
6. Slot tables only grow (never shrink, old ones never freed: ~one table's worth wasted, plus one segment per table for the header line); the rehash stalls the shard's writers (~2.5 ms per 200k entries), go incremental if that shows; `preload_path`; slam defense; `git init`, license, CI, test on released PHP versions.
7. Values stored by side effect inside `stash_scope` dangle after the scope (documented contract).
