# Handover: nocopy

Read this first. `README.md` = user-facing usage. `ARCHITECTURE.md` = how the arena works.
This file = state, how to rebuild everything, what was measured, what to do next.

## Goal

A sellable PHP extension in the style of Relay: a zero-copy shared-memory cache that is a drop-in
for APCu (`apcu_*`, `APCUIterator`, `apc.*` INI) and adds extras (`nocopy_*`). Chosen after reviewing
php-src for weak spots (opcache-style immutable arrays make zero-copy reads possible).

## State (2026-11-04)

Works, not released. Git repo, no license yet. CI: `.github/workflows/ci.yml` (PHP 8.3/8.4/8.5 x amd64/arm64 + ZTS 8.4 + FrankenPHP; not run on GitHub yet: no remote).
Renamed stash -> nocopy (2026-11-04): extension, `nocopy_*` functions, `nocopy.*` INI, `NOCOPY_*` macros, files, Symfony bundle (`Lab\NocopyBundle`, `cache.adapter.nocopy`), Docker images `nocopy-linux`/`nocopy-zts`. The `apcu_*` layer is unchanged. Packagist, PECL and nocopy.dev were free; the GitHub user `nocopy` is taken.

### Performance pass (2026-11-04)
Callgrind per op first (fetch int zero-copy: ~390 instructions, ~110 of them the VM's internal call frame), then `docker/ab.sh 3 "read write parallel"` vs ddf5da4 (Linux Docker arm64, load ~1.6):
- Frameless handlers (PHP 8.4+, `@frameless-function` in the stubs): `apcu_fetch`/`apcu_exists`/`apcu_delete`/`nocopy_get` arity 1, `apcu_store` 1-3, `nocopy_put` 2-3. `apcu_store` needs arity 1: a 1-arg call would pick arity 2 and `$value` has no default (ZEND_UNREACHABLE). gen_stub's legacy output leaves the `#else` (8.3) branch empty: the ZEND_FE lines there are added by hand after regenerating. MINIT adds the handler count to opcache's system id (file cache stores handler indices). Test 040.
- Arena globals `visibility hidden` (no GOT load) and `nocopy_find` takes the shard index directly (`sh - shards` divided by 1280).
- Idle pins (see ARCHITECTURE "Pins"): copy mode no longer re-pins (seq_cst store + fence) on every call.
- Result, copy / zero-copy ns: fetch int 15.5 -> 12.5 / 13.8 -> 12.2, string 100B 21.6 -> 17.4 / 13.4 -> 12.3, 100k distinct keys 44.2 -> 22.8, 10 keys array form 250 -> 145, miss 10.6 -> 9.5, exists 13.7 -> 10.9 / 11.7 -> 10.9; parallel fetch int x8 +16% / +25%. Stores, inc, big array copies within +-2.5% (layout).
- First idle-pin version (writer did not drop its own pins, no cap) broke 029: hot keys kept 1 of 20 (merges evicted around its own pinned segments).
- Suite 8.3/8.4/8.5 + ZTS 8.4, tests/c (TSAN), `docker/stress.sh 5` clean. Not re-run: Symfony smoke, APCu's suite, fpm benchmark.
- Then frameless `apcu_inc`/`apcu_dec` (arity 1, 2) and `nocopy_key_eq` (keys <= 16 bytes compared with two overlapping loads, no memcmp call; exhaustive check in tests/c/stress.c), vs c82d6fc: zero-copy fetches 12.0 -> 11.2 ns, inc 12.5 -> 11.7, exists 10.6 -> 9.9, 10 keys array form -11..12%; miss +0.27 ns (layout: no compare on that path). `apcu_entry`/`nocopy_remember` left framed: callable parsing by hand (trampolines, error wording) for ~1 ns of 22.
- Write path, vs 2f93778: the global atomic `nocopy_a->inserts` (one cache line every writer of every shard bumped) was the parallel-write ceiling. `seq` is now the insertion time in ns from the `clock_gettime` that replaced `time()` in `put_begin`, made strictly increasing per shard in commit (`sh->last_seq`); merge's `born` and `num_inserts` are per shard. Iterator order stays insertion order across shards (ties only within one ns). Also `sh->idx` (no division in `tab_of`/`ghost_cell`) and the write-path helpers (`locate`, `seg_carve`, entry birth/die) inlined: -38 instructions per store.
  Parallel store int x4 / x8 / x16: 30.5 -> 92.5 / 16.2 -> 143.6 / 15.8 -> 123 M ops/s; store+delete 40.2 -> 36.9 ns, inc 11.7 -> 10.9, failing add 31.9 -> 30.3; single stores, ttl churn, readers under writers within noise. The Linux table below ("store int, 4 / 8 / 16 workers") predates this.
- After 73728c2 (Linux Docker 8.5): APCu's suite (krakjoe/apcu fb2c942) 58 pass / 3 fail / 17 skip: apc54_014 (preload_path), apcu_sma_info (one segment per shard), ghbug335 (slam defense). All three are non-goals; ghbug335 fails on ddf5da4 too (the earlier "57 / 2" Linux run had 2 tests fewer: it skipped there). Symfony smoke (framework-bundle 8.x) ok with compat on, off and next to APCu.
- Opcache (8.5 has it built in): suite passes with `opcache.enable_cli=1`, also with the tracing JIT; the opcode dump shows the frameless branches (`JMP_FRAMELESS`); a file-cache round trip gives the same results, and a different handler set (`nocopy.apcu_compat=0`) gets its own system-id directory. Opcache only caches files older than `opcache.file_update_protection` (2 s): set it to 0 when testing with fresh files.
- fpm Symfony app (`REQS=10000`, median of 3), ddf5da4 -> 7813dea: nocopy zero/raw /products 14227 -> 14545, /product/77 14099 -> 14851; every other row within +-4%, which is also how far the unchanged APCu rows moved. Per-call savings of 1-4 ns do not show in a Symfony request: its own work dominates. The gains matter for loops over the cache and parallel writers.
- `MADV_HUGEPAGE` on the arena measured and dropped (2026-11-04): the arena is shmem (MAP_SHARED|ANONYMOUS), so it only acts with `/sys/kernel/mm/transparent_hugepage/shmem_enabled` = advise (Debian/Ubuntu and this VM default to never). With advise (152 MB ShmemHugePages of 512M): 100k distinct keys -3.6% / -6.6% (copy / zero), big copy-mode array fetches -1.7%, everything else flat; parallel fetch int x1..x8 2-4% slower in two runs (fresh forked workers: likely setup cost, not shown for long-lived fpm workers). Not worth a default.
- Slot home from a mixed hash (`nocopy_slot_tag`, 2026-11-04): the home slot was the raw DJBX33A hash's low bits, which for `prefix.N` keys cluster (Python sim, 16k slots at 3/4 fill: hit 215 / miss 1247 / worst 11399 probes, mixed 2 / 8 / 77; random md5 keys unchanged).
  Measured, one shard 4M, `app.Kq2x:product.N` (old -> new ns): at 73% fill hit 28.8 -> 17.8, miss 58.3 -> 15.2, delete+put 107 -> 29; at 25-49% delete+put 30-34 -> 23-25, miss at 49% 11.0 -> 14.3.
  `ab.sh 3 "read write"`: most ops -1..3% (~0.25 ns); `fetch int, 100k distinct keys` 22.1 -> 26.8 (-17.5%), shuffled 54 -> 57.5: raw DJB packed `d0..d99999` into ~800 KB of slot lines, mixed they spread over ~6.4 MB. Kept: sparse tables pay a little, full ones lose their long runs under the lock.
  Test 018 lost its strength with it (no-retry mutation passed 6/6: short runs, tiny race window); it now picks keys homed in the table's first half (a PHP mirror of `nocopy_slot_tag`) and fails 6/6 with the mutation (old test: 3/4).
- Kept copies in copy mode (`nocopy.local_size`, 8M, 2026-11-04; ARCHITECTURE "Fetch modes"): FrankenPHP workers and CLI scripts are one long request, so a private copy can be handed out again while its entry is unchanged (address + seq).
  `ab.sh 3 read` vs e6c6f6d, copy mode: fetch string 10KB 182 -> 12.8 ns, array 10 rows 682 -> 12.8, 1k rows 68 us -> 12.8 ns, 10k rows 711 us -> 12.8 ns, 10k rows + modify 1 (COW) 707 -> 57 us; zero-copy unchanged.
  Costs: fetch int -2%, string 100B -4.5% (0.8 ns; short strings are not kept), `fetch 10 keys` -3.5%. First version kept every copy: 100k distinct strings 37 -> 64 ns, 20k distinct 10-row arrays 360 -> 590 (freeing cold evictees);
  now a copy is kept on its second fetch (`seen`, TinyLFU's doorkeeper) and strings only from 512 B: 37.5 -> 36.3 / 375 -> 387 ns (best of 5, load 3-5: noise). Test 041; 015 caught a double free (slot released, then flushed again).
- Asymmetric fence for pins (`membarrier`, reader fence moved to `reclaim()`) measured and dropped (2026-11-04, Linux Docker arm64, 11 CPUs):
  new pins per fetch (instrumented build): ~0 for hot keys, keys read in order, every zero-copy run and at 512M; 0.92 in copy mode at 32M once the keys read span more than `nsegs/16` segments (100k shuffled keys, 200-key random pages), with or without a writer.
  Removing the reader fence there (seq_cst store + fence -> relaxed + signal fence; unsafe, an upper bound): shuffled 40.9 -> 41.3 ns, pages 61.0 -> 59.0, hot 10.5 = 10.5, 8 parallel readers 50 -> 51 M/s (best of 5): the fence hides under the cache misses those fetches take anyway.
  The writer's price: `reclaim()` reaches its fence once per 399 stores at 32M (6419 at 512M), and `MEMBARRIER_CMD_GLOBAL_EXPEDITED` costs 0.4 / 6.2 / 17 / 46 us with 0 / 4 / 8 / 16 busy registered processes, under the shard lock: ~10 us of stores would pay 6-46 us. Not adopted.
- Gotcha: a container started right after a host-side file write sometimes saw `nocopy.c` truncated ("expected declaration at end of input"): rerun.

### Rebuild of the core (2026-11-03, second half of the day)
- Split: the arena (no PHP; `nocopy_arena.h` public with the read path inline, `nocopy_arena_int.h` internal; `nocopy_arena.c`, `nocopy_evict.c`, `nocopy_lease.c`, `nocopy_owner.c` since 2026-11-04), `nocopy.c` (values, store API in `nocopy_php.h`, `nocopy_*`, module), `nocopy_apcu.c` (only the store API). nocopy.c used to `#include` nocopy_arena.c so the write path inlined (~3 ns per write then); separate units measured 2026-11-04 (`docker/ab.sh 5 read write`): writes equal or 1-4% faster, zero-copy fetches +0.25 ns (layout), so they are separate now. `-fvisibility=hidden`: only `get_module` is exported.
- `tests/c/stress.c` + `tests/c/run.sh`: 8 threads on the arena alone, model-checked, plain and ThreadSanitizer: clean (TSAN on gcc ignores fences: only false alarms possible, none seen). Also builds and passes natively on macOS.
- Shards own fixed segment slices (>= 8 MB, >= 16 segments each): no pool lock, no cross-shard help; an entry must fit in a slice.
- Slot tables fixed (no `grow()`), sized from `apc.entries_hint` / shm size; full table evicts. Backward-shift deletes (no tombstones); readers retry a miss if the shard's `mseq` moved. Test 018 checks that and fails reliably with the retry removed.
- Eviction: Segcache-style merge (sparse segments compacted losslessly; otherwise the 4 oldest, keeping entries read since written, `hot` byte). Replaces evacuate + FIFO evict.
- Locks: still TTAS spin. Robust pthread mutexes measured (C, Linux arm64, 1-16 processes): 59 vs 70 M/s uncontended, 4-13 vs 12-64 M/s contended, also with a trylock spin in front. Instead the owner id is pid + process start time: a recycled pid no longer looks alive (locks and worker slots).
- ZTS: per-thread state in module globals; slot per thread (RINIT/GSHUTDOWN). Tested: whole suite on `php:8.4-zts` + 023 (8 `parallel` threads); FrankenPHP 8.4 classic (zero-copy) and worker mode (copy mode, also with `request_scope=1`) via `docker/frankenphp.sh`.
- Tests: 016/017/018 rewritten (shard slices, fixed table, shifts under readers), 006's fill loop counts evictions (a key read in the loop now survives eviction), 022 (merge keeps read entries), 023 (threads).
- Performance vs the previous core, Linux arm64 Docker, same session, interleaved (`docker/ab.sh`): the host was loaded (load ~18 on 11 cores), timings moved +-10-25% between identical code paths,
  so instruction counts (callgrind, per op) are the reliable figure: fetch -7, store int/string -25, store+delete -49, inc -10, fetch miss +13 (mseq check), failing add +15.
  Timings where they were stable: single-process fetches/miss/exists within +-5%; parallel stores x8/x16 +10%; ttl churn: fewer slow stores (>50 us: ~1150 vs ~1780), slowest store noisy (1.3-5 ms both).
  Not yet measured on a quiet machine: repeat `docker/ab.sh 5` there before quoting numbers (old tree: `git archive 55eec4a`).
- macOS (8.7-dev) not re-run with PHP since the rebuild (no local PHP build left); the arena itself passes tests/c natively on macOS.

### Before the rebuild
- Own tests: `tests/001..021` pass (release and debug builds; 012 needs a PHP with `session`). 015 = 8-process stress: loop it on a debug build after locking changes.
- TTL-bucketed segments with incremental expiry (2026-11-03, see ARCHITECTURE.md); `nocopy_stats()` has `evacuated_bytes` and `shards`.
- Sharded writes (2026-11-03): per-shard locks, a pool lock for segments; see ARCHITECTURE.md "Shards".
- Linux (2026-11-03, Docker `php:8.4-fpm`/`php:8.5-fpm`, arm64, `docker/`): did not compile (glibc's `unshare(2)` clashed: renamed `unshare_zval`);
  then 015 failed 1 in ~150 runs and could hang: segment walks read entries of a segment freed and reused by another shard after their own last unlink (fixed, see ARCHITECTURE invariants).
  Now 21/21 on both, 015 loops 400 (8.4) / 250 (8.5) clean.
- Dead lock holders (2026-11-03): locks store the holder's pid; waiters take over a lock whose holder is dead or a zombie (021: 30 random kill -9s of a writer, parent writes before reaping; old code deadlocked). Contention benchmarks unchanged. Since 2026-11-04 a takeover repairs the shard instead of leaking (see ARCHITECTURE "Locks"); 037 on the old code: ~60 of 242 segments stuck after 40 kills, now none.
- FIFO eviction when full (2026-11-03): APCu wipes everything, nocopy used to refuse every write (187k of 200k stores failed in an 8M arena). Tests 011/015 accept a miss only if `expunges` moved since the store.
- Value strings are no longer hashed when stored (2026-11-03): `store string 100B` 76 -> 49 ns (APCu 57).
- Growable slot tables (2026-11-03): a shard's table doubles into pool segments when it fills; `nocopy_stats()` has `slots`. 018 loops clean 60x on debug.
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
3. Build nocopy out of tree (copy sources to a build dir, then): `phpize && ./configure --with-php-config=<prefix>/bin/php-config && make`.
   After restoring/replacing a source file, force a rebuild (`rm -f nocopy.lo .libs/nocopy.o modules/nocopy.so`): stale builds fooled us twice.
4. APCu for comparison: `git clone https://github.com/krakjoe/apcu`, build the same way. nocopy and APCu cannot be loaded together.
5. Symfony Cache tests: clone `symfony/cache` (split repo), `composer install` (use `--ignore-platform-req=php`), run `vendor/bin/phpunit Tests/Adapter/ApcuAdapterTest.php` with `-d extension=nocopy.so -d apc.enable_cli=1`.
6. Real app benchmark: `bench/sfapp/` holds only `src/`, `public/`, `composer.json`. Copy to a work dir, `composer install --ignore-platform-req=php`, `composer dump-autoload -o`.

## Run things

| What | Command |
|---|---|
| own tests | `TEST_PHP_EXECUTABLE=<php> <php> run-tests.php -n -d extension=$PWD/modules/nocopy.so tests` |
| APCu suite | `PHP=<php> NOCOPY_SO=<so> scripts/apcu-suite.sh [workdir]` (expect 59/61) |
| micro benchmarks vs APCu | `PHP=.. NOCOPY_SO=.. APCU_SO=.. bench/all.sh` (release builds only) |
| Symfony adapter benchmarks | `PHP=.. NOCOPY_SO=.. APCU_SO=.. SF_AUTOLOAD=<symfony/cache>/vendor/autoload.php bench/symfony-all.sh` |
| hit ratio of one workload (zipf / loop, footprint, one-hit wonders) | `php -d extension=<so> -d apc.shm_size=4M bench/hitratio.php zipf 1.5 0` |
| stampede (8 workers, ttl 2 s, 50 ms callback) | `docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/stampede.sh` |
| real Symfony app (php -S, 8 workers, ab) | `PHP=.. NOCOPY_SO=.. APCU_SO=.. APP=<app dir> bench/sfapp.sh` |

Gotchas: zsh scripts (`${=VAR}` for word splitting); `-n` to ignore php.ini; the benchmark scripts guard against a
misconfigured APCu `shm_size`; in macOS `sed -i ''`; run-tests.php is the one installed with PHP, not APCu's.

## Measured results on Linux (2026-11-03, Docker on the same Mac: OrbStack VM, 11 CPUs, arm64, PHP 8.5.12 official image, APCu from pecl)

Run: `docker build --build-arg PHP=8.5 -t nocopy-linux:8.5 docker`, then `docker/bench.sh` (micro, median of 3) and `docker/fpm-bench.sh` (Symfony app, nginx + php-fpm, 8 static workers, ab -c 8, 20000 requests, median of 3).

| micro (ns/op unless noted) | APCu | nocopy copy | nocopy zero-copy |
|---|---|---|---|
| fetch int / string 100B | 28.6 / 34.9 | 15.5 / 19.9 | 13.0 / 13.0 |
| fetch array 1k rows | 202 us | 91 us | 13 ns |
| store int / string 100B / array 1k rows | 55.7 / 81.8 / 183 us | 29.3 / 34.6 / 51 us | same as copy |
| parallel fetch int, 8 workers (M ops/s) | 3.8 | 154 | 221 |
| store int, 4 / 8 / 16 workers (M ops/s) | 10.2 / 10.9 / 8.8 | 27.6 / 15.9 / 15.4 | same |
| 4 readers fetch int while 4 writers run (M ops/s) | 7.2 | 93 | 131 |
| ttl churn: M stores/s, slowest store | 0.89, 133 ms | 2.81, 3.1 ms | 2.86, 2.7 ms |

CORRECTION of the macOS numbers: APCu's write throughput under contention does NOT collapse on Linux (10.9 M ops/s with 8 writers vs 0.26 on macOS): the macOS "20-40x vs APCu" write rows were a macOS lock artifact.
On Linux nocopy writes are 1.5-2.7x APCu with several writers, ~2x single-process; reads are where the big gap is (40-60x with 8 readers, APCu's read lock).

| fpm, requests/s | /hello | /products | /product/77 | /validate |
|---|---|---|---|---|
| APCu, Symfony default pool | 14108 | 2975 | 3020 | 7803 |
| nocopy, default pool | 12607 | 3136 | 3112 | 7885 |
| APCu, raw pool | 15059 | 4116 | 4087 | 7892 |
| nocopy copy, raw pool | 14210 | 4553 | 4362 | 8127 |
| **nocopy zero-copy, raw pool** | 12702 | **14231** | **13959** | 8401 |

Zero-copy raw pool: 4.8x APCu's default pool and 3.5x APCu raw on cache-heavy pages (macOS showed 2.1x / 1.7x). `request_scope=auto` picked zero-copy under fpm as intended.
Open: `/hello` (barely touches the cache) is 6-16% lower with nocopy under fpm, but an in-process loop (fresh kernel per request) shows no gap (nocopy 179/167 us, APCu 168/181): likely noise at ~14k req/s with ab as the limit; repeat with wrk before trusting either way.

Key dedupe (2026-11-03, Linux Docker arm64, `docker/ab.sh 5 read write`, best of 5, quiet host, load ~2): equal array keys stored once per entry, copied out once per fetch.
Copy mode: fetch array 10 rows 796 -> 714 ns, 1k rows 86.5 -> 70.5 us (also with unserialized keys: 88.8 -> 73.0), 10k rows 909 -> 727 us (-20..25%). Arena bytes for `rows(1000)` 648 KB -> 520 KB (-20%).
Price: store array 10 rows 511 -> 594 ns, 1k rows 49.3 -> 58.1 us (+15-17%: two table probes per key). Zero-copy reads, scalars, objects: within noise.
`fetch 10 keys (array form)` copy reads 185 -> 205 ns (+9-11%) in every run, but callgrind counts +0.6% instructions (+2.5 per key): code alignment, not chased.
The table first sat inside the module globals (8 KB): that alone moved `apcu_entry hit` +6%; it is a separate allocation now.


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

The reader drop is CPU capacity, not nocopy: 4 plain PHP busy loops that never touch nocopy cut the same readers to 91-100 M ops/s on this 5P+6E Mac
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
| nocopy, default pool | 5595 | 2743 | 2784 | 4404 |
| APCu, raw pool | 5720 | 3291 | 3338 | 4488 |
| nocopy copy, raw pool | 5793 | 3499 | 3446 | 4366 |
| **nocopy zero-copy, raw pool** | 5435 | **5622** | **5640** | 4655 |

KEY FINDING: Symfony's default `cache.adapter.apcu` wires `cache.default_marshaller`, so values are serialized to strings in PHP
before the extension sees them; nocopy then gives ~2%. The gain (~2.1x vs default, ~1.7x vs APCu raw) needs a pool without a marshaller
(`new ApcuAdapter($ns, 0, null, null)`, see `bench/sfapp/src/Kernel.php`). A Symfony bundle (`cache.adapter.nocopy`) that does this is the obvious product piece.
Earlier "zero-copy write is 50-100x slower" rows were a bug (retire-list pile-up), fixed by the segment rewrite; do not trust old result files.

## Key decisions (why things are the way they are)

- Zero-copy needs lifetime guarantees; they come from per-worker pin bitmaps over segments (see ARCHITECTURE.md). Replaced an epoch + per-entry retire list that piled up garbage.
- `nocopy.request_scope=auto`: zero-copy for the whole request only under fpm/cgi/apache/litespeed/cli-server; CLI and workers use copy mode.
- Objects/shared references/arrays with refcount>1 references fall back to `serialize()`; resources are refused with APCu's warning.
- `serialize`/`unserialize` inside the extension need `BG(serialize_lock)++` (re-entrancy; APCu bug #76145).
- Per-entry hit counts are intentionally unsupported (would be a shared write per read).
- Comments in code: one short line only (user preference).
- Slot tables are fixed rather than growable: no stall, no frozen tables, no table re-check on hits; the price is evicting by entry count when entries are tiny (`apc.entries_hint`).
- `nocopy_remember`'s compute time is the header field `nocopy_entry.user`, not the value zval's `u2` (2026-11-04); `created` became 32-bit so the header stays 48 bytes. Instructions per op unchanged (callgrind +0.08%); timings moved 0.3-2 ns either way (layout).
- Sizing INI is `nocopy.*` in both modes; `apc.shm_size`/`apc.entries_hint`/`apc.ttl` are aliases under the APCu layer (2026-11-04, test 038). README lists where nocopy differs from APCu by design ("same API, not the same layout"). APCu's suite unchanged: 57 pass, apc54_014 and apcu_sma_info fail. Symfony smoke (8.5, compat on/off/next to APCu) passes; 8.3/6.4 and 8.4/7.4 not re-run.
- Single-flight leases live in a fixed per-shard table, not in the cache (2026-11-04): they used slots and segments, showed up in `APCUIterator`, were wiped by `apcu_clear_cache`, and dropping by key could drop a lease another worker had retaken. `docker/stampede.sh` unchanged (1 callback run per expiry, same req/s).
- Module boundaries (2026-11-04, one refactor): a write is a `nocopy_put` the arena owns (`nocopy_put_begin` locks, allocates, sets the header, drops the lock for big blocks with `wip` recorded; `nocopy_put_commit` relocks, publishes, unlocks), so nocopy.c never handles the lock protocol;
  a merge moves an entry by `memcpy` + `nocopy_rebase` (pointer shift) instead of re-persisting it under the lock with module globals (no size invariant on relocation, faster); sizing/persisting/copy-out take explicit contexts instead of `NOCOPY_G(cursor)`/`kgen`;
  `nocopy_opts` {ttl, add_only, objects: refuse|serialize} and a `nocopy_status` replace boolean parameters and NULL-for-everything; `nocopy_apcu.c` uses `nocopy_info`/`nocopy_scan`/`nocopy_add_long`/`nocopy_cas_long` instead of locks and arena fields;
  merge is split into pick_sparse / pick_oldest / move_survivors / evict_rest; a segment remembers its TTL bucket (relocate no longer fakes `now = created`). Test 039 (packed, nested, serialized values moved; fails with a no-op rebase).
  `docker/ab.sh 5` vs 7222dd9 (Linux Docker arm64, load ~1.5): copy fetches of arrays/long strings +2..6%, scalar stores +0.5..1.5%, array stores -1..2%, `inc counter` -6.5% (11.5 -> 12.3 ns: one more call, apcu_inc now goes through nocopy.c), `exists` -1..2%, parallel zero-copy fetches -1.6..2.7%, ttl churn and contention within noise.
  Suite passes on 8.3/8.4/8.5/ZTS 8.4/amd64, tests/c (TSAN) clean, `docker/stress.sh 30` clean, Symfony smoke (8.5, compat on/off/next to APCu) ok. APCu's own suite not re-run (needs a fresh clone of krakjoe/apcu).
- No way to release zero-copy pins before the request ends (`nocopy_checkpoint`, `nocopy_scope` and FrankenPHP `request_scope=1` zero-copy were removed 2026-11-03): each relied on userland not keeping a value, and a broken promise was a use-after-free. Long-lived workers get copy mode.

## Known gaps / next steps (rough priority)

0. Merge eviction keeps read entries only up to one segment per 4 merged (the most-read first, test 029); tune MERGE_N / the budget if hit rates in a full cache need it (`nocopy_stats()` has `evicted` / `expired` now). Ghost of evicted tags (test 033), `bench/hitratio.php`, no-ghost build given the ghost's 4 B/slot as cache (4M / 32M, % hits ghost vs none): zipf 1.5x / 3x the cache 91.2/91.3, 81.5/82.3 / 92.9/92.4, 85.3/84.8 (noise); zipf + 1 one-hit wonder per request 77.6/75.2 / 79.6/77.9; loop 1.25x 25/0 / 22/0; loop 1.8x 0/0; loop 0.25x + 5 one-hit wonders per request 56/0 / 56/0, at 0.4x 0/0. Ghost shape measured (2026-11-04, Linux Docker, `bench/hitratio.php`, % hits at 4M / 32M; none / direct-mapped (kept) / 2-way / 2x tags, same table bytes except 2x):
   zipf 1.5x, 3x: 91.4/91.1/91.1/90.9, 82.5/81.5/81.5/81.2 / 93.0/92.9/92.9/92.7, 85.7/85.3/85.3/84.5 (the ghost costs <= 1 point);
   zipf + 1 one-hit wonder per request, 1.5x / 3x: 75.3/77.6/76.9/77.7, 69.3/71.4/70.8/71.6 / 78.3/79.7/80.1/80.6, 73.1/74.6/74.7/75.3;
   loop 1.25x: 0/25/25/25 / 0/22.2/22.2/25; loop 0.25x + 5 one-hit wonders: 0/56.3/50.8/58 / 0/55.6/58.1/57.5; loop 1.8x and 0.4x + 5: 0 everywhere.
   Kept direct-mapped 1x: 2-way is no better, 2x gains 0-3 points on mixed workloads and loses up to 0.8 on pure zipf for 4 B/slot more; dropping the ghost loses 2 points on mixed and everything on scans. A failing `apcu_add` costs 15 instructions more than before (not chased).
1. Symfony bundle: done in `symfony/` (`cache.adapter.nocopy`, smoke test passes on PHP 8.3/8.4/8.5 with Symfony 6.4 and 8.1). Rewritten on `nocopy_*` (`NocopyAdapter`, versioned clear), so it works with `nocopy.apcu_compat=0` and next to APCu; `bench/sfapp`'s raw pool uses it under nocopy. fpm run with `NocopyAdapter` (2026-11-03, load 37 -> 12 during the run): zero-copy /products 13482, /product/77 12865 req/s vs APCu raw 3991 / 3949 (3.4x / 3.3x) and APCu default pool 2279 / 2392 (5.9x / 5.4x); /hello and /validate within noise of APCu. fpm run with the bundle (2026-11-03, load 9 -> 46 during the run: shape only): zero-copy /products 9809, /product/77 12328 req/s vs APCu raw 3764 / 2804. CI: `symfony` job (`docker/symfony.sh`: 6.4/8.3, 7.4/8.4, 8.x/8.5; compat on, off, next to APCu). Open: Packagist name, a quiet rerun.
2. Linux runs exist now (Docker, above); still: a real (non-VM) x86_64 box for numbers, wrk instead of ab; push to GitHub so CI actually runs.
3. Shards: expiry of a shard without writes waits for memory pressure. Fixed slices: one hot shard cannot borrow from idle ones.
4. Compaction runs under the global lock (~ms stall); RSS drifts to `apc.shm_size` because segments are reused only under pressure; `MADV_HUGEPAGE`. A one-pass `copy_out` was considered: `zend_array_dup` of an immutable array is already one memcpy, the cost was per-key allocation (fixed by key dedupe).
5. Milestone 3 features that justify a price: tags, cross-server invalidation (single-flight `apcu_entry` + `nocopy_remember` with XFetch: done, tests 030/031; APCu's own suite not re-run since). Bigger idea: Redis client-side cache tier (Relay model, RESP3 tracking), where the gain is 100x not 2x.
6. Packaging: license and vendor name undecided (`composer.json` is still the ext_skel skeleton); PIE install needs a GitHub release.
   `preload_path`; slam defense; license.
