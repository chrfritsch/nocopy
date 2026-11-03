# Handover: stash

Read this first. `README.md` = user-facing usage. `ARCHITECTURE.md` = how the arena works.
This file = state, how to rebuild everything, what was measured, what to do next.

## Goal

A sellable PHP extension in the style of Relay: a zero-copy shared-memory cache that is a drop-in
for APCu (`apcu_*`, `APCUIterator`, `apc.*` INI) and adds extras (`stash_*`). Chosen after reviewing
php-src for weak spots (opcache-style immutable arrays make zero-copy reads possible).

## State (2026-11-03)

Works, not released. Git repo, no license yet. CI: `.github/workflows/ci.yml` (PHP 8.3/8.4/8.5 x amd64/arm64 + ZTS 8.4 + FrankenPHP; not run on GitHub yet: no remote).

### Rebuild of the core (2026-11-03, second half of the day)
- Split: `stash_arena.c/.h` (no PHP; read path inline in the header), `stash.c` (values, `stash_*`, module; `#include`s stash_arena.c so the write path inlines: one unit), `stash_apcu.c`. `-fvisibility=hidden`: only `get_module` is exported.
- `tests/c/stress.c` + `tests/c/run.sh`: 8 threads on the arena alone, model-checked, plain and ThreadSanitizer: clean (TSAN on gcc ignores fences: only false alarms possible, none seen). Also builds and passes natively on macOS.
- Shards own fixed segment slices (>= 8 MB, >= 16 segments each): no pool lock, no cross-shard help; an entry must fit in a slice.
- Slot tables fixed (no `grow()`), sized from `apc.entries_hint` / shm size; full table evicts. Backward-shift deletes (no tombstones); readers retry a miss if the shard's `mseq` moved. Test 018 checks that and fails reliably with the retry removed.
- Eviction: Segcache-style merge (sparse segments compacted losslessly; otherwise the 4 oldest, keeping entries read since written, `hot` byte). Replaces evacuate + FIFO evict.
- Locks: still TTAS spin. Robust pthread mutexes measured (C, Linux arm64, 1-16 processes): 59 vs 70 M/s uncontended, 4-13 vs 12-64 M/s contended, also with a trylock spin in front. Instead the owner id is pid + process start time: a recycled pid no longer looks alive (locks and worker slots).
- ZTS: per-thread state in module globals; slot per thread (RINIT/GSHUTDOWN). Tested: whole suite on `php:8.4-zts` + 023 (8 `parallel` threads); FrankenPHP 8.4 classic (zero-copy) and worker mode (`auto`: copy mode; `request_scope=1`: zero-copy, pins released per handled request) via `docker/frankenphp.sh`.
- Tests: 016/017/018 rewritten (shard slices, fixed table, shifts under readers), 006's fill loop counts evictions (a key read in the loop now survives eviction), 022 (merge keeps read entries), 023 (threads).
- Performance vs the previous core, Linux arm64 Docker, same session, interleaved (`docker/ab.sh`): the host was loaded (load ~18 on 11 cores), timings moved +-10-25% between identical code paths,
  so instruction counts (callgrind, per op) are the reliable figure: fetch -7, store int/string -25, store+delete -49, inc -10, fetch miss +13 (mseq check), failing add +15.
  Timings where they were stable: single-process fetches/miss/exists within +-5%; parallel stores x8/x16 +10%; ttl churn: fewer slow stores (>50 us: ~1150 vs ~1780), slowest store noisy (1.3-5 ms both).
  Not yet measured on a quiet machine: repeat `docker/ab.sh 5` there before quoting numbers (old tree: `git archive 55eec4a`).
- macOS (8.7-dev) not re-run with PHP since the rebuild (no local PHP build left); the arena itself passes tests/c natively on macOS.

### Before the rebuild
- Own tests: `tests/001..021` pass (release and debug builds; 012 needs a PHP with `session`). 015 = 8-process stress: loop it on a debug build after locking changes.
- TTL-bucketed segments with incremental expiry (2026-11-03, see ARCHITECTURE.md); `stash_stats()` has `evacuated_bytes` and `shards`.
- Sharded writes (2026-11-03): per-shard locks, a pool lock for segments; see ARCHITECTURE.md "Shards".
- Linux (2026-11-03, Docker `php:8.4-fpm`/`php:8.5-fpm`, arm64, `docker/`): did not compile (glibc's `unshare(2)` clashed: renamed `unshare_zval`);
  then 015 failed 1 in ~150 runs and could hang: segment walks read entries of a segment freed and reused by another shard after their own last unlink (fixed, see ARCHITECTURE invariants).
  Now 21/21 on both, 015 loops 400 (8.4) / 250 (8.5) clean.
- Dead lock holders (2026-11-03): locks store the holder's pid; waiters take over a lock whose holder is dead or a zombie (021: 30 random kill -9s of a writer, parent writes before reaping; old code deadlocked). Leaks ~0.5 segment per takeover. Contention benchmarks unchanged.
- FIFO eviction when full (2026-11-03): APCu wipes everything, stash used to refuse every write (187k of 200k stores failed in an 8M arena). Tests 011/015 accept a miss only if `expunges` moved since the store.
- Value strings are no longer hashed when stored (2026-11-03): `store string 100B` 76 -> 49 ns (APCu 57).
- Growable slot tables (2026-11-03): a shard's table doubles into pool segments when it fills; `stash_stats()` has `slots`. 018 loops clean 60x on debug.
- Review fixes (2026-11-03): arena strings no longer `IS_STR_PERMANENT` (PCRE kept them as process-wide cache keys);
  pins dropped in post-deactivate, not RSHUTDOWN (session's RSHUTDOWN serialized `$_SESSION` after ours: test 012 segfaulted);
  `apcu_cache_info`/`APCUIterator`/`apcu_sma_info` no longer allocate under the lock (OOM inside it deadlocked every worker: test 013); ZTS builds refused with `#error`.
- APCu's own .phpt suite on Linux (PHP 8.5, APCu fb2c942, 2026-11-03): 57 pass / 2 fail (apc54_014: preload_path, non-goal; apcu_sma_info). Both apcu_sma_info and not_enough_shm passed before the core rebuild (55eec4a) and broke with it: not_enough_shm (error wording) is fixed, test 026. apcu_sma_info expects one segment with one free block after one store: it now reports one segment per shard (`num_seg` = shards, `seg_size` = slice, `block_lists` per slice, offsets from the slice start, test 027), since an entry only fits into its own shard's free space; it passes at <= 8M (one shard), not at the default 32M (4). Left as is (it asserts APCu's layout). Earlier, macOS 8.7-dev: 59 pass / 2 fail. Failures are non-goals: `apc54_014` (preload_path), `ghbug335` (slam defense).
- Symfony Cache: `Tests/Adapter/ApcuAdapterTest.php` passes like real APCu (154 tests / 285 assertions / 6 skipped, 2026-11-03, PHP 8.7-dev macOS).
  It is the only file in the suite that touches the extension (without it loaded: 154 skipped, the other ~6700 tests still run), so the full suite adds nothing: run just this file before releases and for the Symfony bundle.
- PHP 8.3 support (2026-11-03): stub regenerated with `@generate-legacy-arginfo 80300`, `php_pcre_match_impl` signature guarded; 001 accepts 8.3's `debug_zval_dump` (no "packed").

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

## Measured results on Linux (2026-11-03, Docker on the same Mac: OrbStack VM, 11 CPUs, arm64, PHP 8.5.12 official image, APCu from pecl)

Run: `docker build --build-arg PHP=8.5 -t stash-linux:8.5 docker`, then `docker/bench.sh` (micro, median of 3) and `docker/fpm-bench.sh` (Symfony app, nginx + php-fpm, 8 static workers, ab -c 8, 20000 requests, median of 3).

| micro (ns/op unless noted) | APCu | stash copy | stash zero-copy |
|---|---|---|---|
| fetch int / string 100B | 28.6 / 34.9 | 15.5 / 19.9 | 13.0 / 13.0 |
| fetch array 1k rows | 202 us | 91 us | 13 ns |
| store int / string 100B / array 1k rows | 55.7 / 81.8 / 183 us | 29.3 / 34.6 / 51 us | same as copy |
| parallel fetch int, 8 workers (M ops/s) | 3.8 | 154 | 221 |
| store int, 4 / 8 / 16 workers (M ops/s) | 10.2 / 10.9 / 8.8 | 27.6 / 15.9 / 15.4 | same |
| 4 readers fetch int while 4 writers run (M ops/s) | 7.2 | 93 | 131 |
| ttl churn: M stores/s, slowest store | 0.89, 133 ms | 2.81, 3.1 ms | 2.86, 2.7 ms |

CORRECTION of the macOS numbers: APCu's write throughput under contention does NOT collapse on Linux (10.9 M ops/s with 8 writers vs 0.26 on macOS): the macOS "20-40x vs APCu" write rows were a macOS lock artifact.
On Linux stash writes are 1.5-2.7x APCu with several writers, ~2x single-process; reads are where the big gap is (40-60x with 8 readers, APCu's read lock).

| fpm, requests/s | /hello | /products | /product/77 | /validate |
|---|---|---|---|---|
| APCu, Symfony default pool | 14108 | 2975 | 3020 | 7803 |
| stash, default pool | 12607 | 3136 | 3112 | 7885 |
| APCu, raw pool | 15059 | 4116 | 4087 | 7892 |
| stash copy, raw pool | 14210 | 4553 | 4362 | 8127 |
| **stash zero-copy, raw pool** | 12702 | **14231** | **13959** | 8401 |

Zero-copy raw pool: 4.8x APCu's default pool and 3.5x APCu raw on cache-heavy pages (macOS showed 2.1x / 1.7x). `request_scope=auto` picked zero-copy under fpm as intended.
Open: `/hello` (barely touches the cache) is 6-16% lower with stash under fpm, but an in-process loop (fresh kernel per request) shows no gap (stash 179/167 us, APCu 168/181): likely noise at ~14k req/s with ab as the limit; repeat with wrk before trusting either way.

## Measured results (macOS, release -O2, single machine: treat as indicative)

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

FIFO eviction + unhashed value strings (median of 3, before -> after): store string 100B 76 -> 49 ns, store object 354 -> 288, store array 1k rows 90.5 -> 84.4 us;
500 B value, same key 242 -> 72 ns (APCu 45), unique keys 375 -> 163 (APCu 148). Eviction itself: ~17 ns per store in a full 8M arena.
`ttl churn` turned into a full-cache benchmark because stores got faster (1.65 -> 2.97 M/s): the unexpired ttl=1 data alone fills 512M,
so it evicts (~300 segments) and evacuates ~2.1 GB in 5 s (stores > 50 us 2090 -> 4784, slowest 0.5 -> 2.7 ms). At 1024M: 1.8 MB evacuated, 497 slow stores.
Next: when truly full, evacuation copies data that is about to expire anyway; evicting before evacuating in that case would cut the copying.

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
- Slot tables are fixed rather than growable: no stall, no frozen tables, no table re-check on hits; the price is evicting by entry count when entries are tiny (`apc.entries_hint`).
- FrankenPHP worker mode defaults to copy mode: long-lived apps keep fetched values across requests, which would dangle after a checkpoint.

## Known gaps / next steps (rough priority)

0. Merge eviction keeps read entries only up to one segment per 4 merged (the most-read first, test 029); tune MERGE_N / the budget if hit rates in a full cache need it (`stash_stats()` has `evicted` / `expired` now). A failing `apcu_add` costs 15 instructions more than before (not chased).
1. Symfony bundle: done in `symfony/` (`cache.adapter.stash`, smoke test passes on PHP 8.3/8.4/8.5 with Symfony 6.4 and 8.1). Rewritten on `stash_*` (`StashAdapter`, versioned clear), so it works with `stash.apcu_compat=0` and next to APCu; `bench/sfapp`'s raw pool uses it under stash. fpm run with `StashAdapter` (2026-11-03, load 37 -> 12 during the run): zero-copy /products 13482, /product/77 12865 req/s vs APCu raw 3991 / 3949 (3.4x / 3.3x) and APCu default pool 2279 / 2392 (5.9x / 5.4x); /hello and /validate within noise of APCu. fpm run with the bundle (2026-11-03, load 9 -> 46 during the run: shape only): zero-copy /products 9809, /product/77 12328 req/s vs APCu raw 3764 / 2804. CI: `symfony` job (`docker/symfony.sh`: 6.4/8.3, 7.4/8.4, 8.x/8.5; compat on, off, next to APCu). Open: Packagist name, a quiet rerun.
2. Linux runs exist now (Docker, above); still: a real (non-VM) x86_64 box for numbers, wrk instead of ab; push to GitHub so CI actually runs.
3. Lock takeover: segments leaked by takeovers are never recovered.
   Copy mode: dedupe array keys at persist time (pointer-keyed; PDO rows share key strings) and copy out in one pass.
   Shards: expiry of a shard without writes waits for memory pressure. Fixed slices: one hot shard cannot borrow from idle ones.
4. Compaction runs under the global lock (~ms stall); RSS drifts to `apc.shm_size` because segments are reused only under pressure; `MADV_HUGEPAGE`; faster `copy_out` (one pass).
5. Milestone 3 features that justify a price: tags, single-flight `apcu_entry`, cross-server invalidation. Bigger idea: Redis client-side cache tier (Relay model, RESP3 tracking), where the gain is 100x not 2x.
6. Packaging: license and vendor name undecided (`composer.json` is still the ext_skel skeleton); PIE install needs a GitHub release.
   `preload_path`; slam defense; license.
7. Values stored by side effect inside `stash_scope` dangle after the scope (documented contract).
