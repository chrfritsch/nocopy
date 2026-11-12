# `./dev` commands

`./dev` runs everything in Docker; the images are built on first use and reused after (`docker build` keeps its cache).
Every command copies the sources into the container and builds from clean; the checkout is mounted read-only.
CI ([`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)) calls the same commands.

`PHP=8.4` (or `8.5`, the default) picks the PHP version for the `nocopy-linux:<PHP>` image.

## Tests

| Command | What it runs | Image |
|---|---|---|
| `./dev test [tests/0NN.phpt …]` | `.phpt` suite (or the given tests) on all cores, with a Valkey server for the 05x tests | `nocopy-linux:$PHP` |
| `./dev novalkey [tests …]` | same, built with `--disable-nocopy-valkey` | `nocopy-linux:$PHP` |
| `./dev asan [tests …]` | same, built with AddressSanitizer + UBSan (libasan preloaded into the uninstrumented php, `USE_ZEND_ALLOC=0`, no leak check) and `-DNOCOPY_DEBUG`: nocopy's own assertions (`NC_ASSERT`: a write filled exactly the planned bytes and consumed the tape; every pointer in the written block stays in it, or is the empty array or an atom; keys carry their hash; no graph marker reaches userland), which a release PHP's `ZEND_ASSERT` drops | `nocopy-linux:$PHP` |
| `./dev debug [tests …]` | same as `asan`, on PHP built with `--enable-debug` (pcntl, posix, session, igbinary): the engine's own assertions, among them a refcount changed on an immutable array or interned string (`ZEND_RC_DEBUG`) and hash-table consistency. `./dev debug fuzz [iterations] [seed]`: the fuzzer on that build. The image builds PHP once (minutes), then is cached | `nocopy-debug:$PHP` |
| `./dev leaks [tests …]` | the suite on that debug PHP without ASan, so the Zend allocator runs: it reports memory a request allocated with `emalloc` and did not free (`=== Total N memory leaks detected ===`), which fails the test, CLI and `php -S` alike (`serve()` passes the server's report on). Built with `-DNOCOPY_DEBUG`. `./dev leaks fuzz [iterations] [seed]`: the fuzzer there. Persistent memory (`malloc`, `pemalloc`) is not covered | `nocopy-debug:$PHP` |
| `./dev clang [tests …]` | same, built with clang (warnings gcc does not give; `-Werror` too) | `nocopy-linux:$PHP` |
| `./dev zts [tests …]` | suite on thread-safe PHP 8.4 with ext-parallel | `nocopy-zts:8.4` |
| `./dev stress [loops=30]` | concurrency tests (011, 015, 018, 021, 022) in a loop on a release build, stops at the first failure or hang | `nocopy-linux:$PHP` |
| `./dev c [seconds=3]` | the arena alone (`tests/c/`) from 8 threads, plain and ThreadSanitizer, then (stress) a clear must give every segment back; then the repair, atom, merge and crash tests and the interleaving explorer (`sched.c`) | `nocopy-linux:$PHP` |
| `./dev fuzz [iterations=20000] [seed]` | `tests/fuzz.php` on an ASan + UBSan build, copy mode then zero-copy: random values (every object kind, dates, shared references, cycles, deep nesting) stored over 1024 keys in a 4 MB arena, so merges move them and evict; every fetch must `serialize()` as `unserialize(serialize())` of what was stored; after each mode a fresh process clears the arena and every segment must be back in the pool (`tests/empty.inc`). Prints the seed; the same iterations and seed replay the run, `FUZZ_VALUE=<iteration>` dumps the value one iteration made | `nocopy-linux:$PHP` |
| `./dev shm` | CLI and php-fpm share one cache by name, survive reloads and restarts, churn without misses; then the same with Valkey; then CLI and Apache mod_php (parent as root, children `www-data`) | `nocopy-linux:$PHP`, `php:$PHP-apache` |
| `./dev pie` | `pie install` (PIE 1.5.5) from the committed tree as a path repository: default, `--disable-nocopy-valkey`, `--disable-nocopy-lto`; each loads and round-trips a value | `nocopy-linux:$PHP` |
| `./dev frankenphp` | FrankenPHP classic and worker mode; then the `shm` checks under FrankenPHP | `dunglas/frankenphp:1-php8.4` |
| `./dev symfony [version=8.*]` | Symfony bundle smoke test, alone and next to APCu; installs `symfony/framework-bundle` into `.cache/` | `nocopy-linux:$PHP` |
| `./dev apcu` | APCu's own test suite against the polyfill; fails on any failure beyond the 13 known ones | `nocopy-linux:$PHP` |
| `./dev drupal` | Drupal module PHPUnit tests in 3 modes: shared (CLI: copy mode), Valkey, no shared cache. Drupal checkout kept in volume `nocopy-drupal-test` | `nocopy-drupal` |
| `./dev tidy` | clang-tidy over the extension's C sources, checks in `.clang-tidy`; any finding fails | `nocopy-linux:$PHP` |
| `./dev layers` | the layers' boundaries (`scripts/check-layers.sh`). Arena: include order, `layout.h` only in `arena.c`/`layout.c`, outside `arena/` only `arena/nocopy_arena.h` and no `ar_` name. Values: no module state (`nocopy_php.h`, `NOCOPY_G`) in the value units, `nocopy_value_int.h` only in them, no allocator or user-code call in `nocopy_write.c`. Above: no module state in `nocopy_kept.c|h`, no `nocopy_value_int.h` name outside the value units, no arena data operation in `nocopy.c` (in `nocopy_valkey.c` only `nocopy_clear`), Valkey only through `nocopy_valkey.h` (`nocopy.c`, `nocopy_php.h`), `*_local`/`nocopy_store_each` only in the store and the tier | host |
| `./dev ci` | `layers tidy test novalkey asan debug leaks clang stress c fuzz "debug fuzz" "leaks fuzz" shm pie zts frankenphp symfony apcu`, stops at the first failure | |

## Benchmarks

| Command | What it runs |
|---|---|
| `./dev bench [groups]` | `bench/run.php` groups (default all: read write parallel mixed contend ttl) ×3 for APCu, nocopy copy and zero-copy; prints medians. Env: `ONLY` (regex on measurement names, the rest skipped) |
| `./dev ab [ref=main] [reps=3] [groups]` | `docker/ab.sh`: `ref` (via `git archive`, in `.cache/ab-old`) vs the working tree, interleaved in one container, best of `reps`, copy and zero mode. Env: `ONLY` |
| `./dev fpbench` | the `sfbench` app in FrankenPHP worker mode (8 workers): APCu, PHP files, nocopy with and without kept copies (`WORKERS`, `REQS`, `CONC`) |
| `./dev sfbench` | `bench/sfapp` under nginx + php-fpm with Symfony's recommended production PHP settings: APCu (with and without Symfony's marshaller) vs the bundle, 4 pages, median of 3. Env: `WORKERS`, `REQS`, `CONC` |
| `./dev opcache` | `bench/opcache.php`: nocopy vs values kept as PHP files in OPcache (Symfony's `PhpFilesAdapter`), fetch, store, OPcache waste per rewrite |
| `./dev tail [workers=8] [secs=10] [scenarios]` | `bench/tail.php`: get/put latency p50..p99.99 and max under `fit`, `evict`, `contend`; APCu, nocopy copy and zero-copy (`bench/tail.php` alone: `TAIL_IDLE=n` adds n sleeping slot holders) |
| `./dev big ["1G 4G"] [secs=20]` | `bench/big.php`: arena create time, fill until eviction, then 4 readers + 4 writers of new keys: get/put percentiles, pauses over 1/10/50 ms, MB moved per eviction round |
| `./dev soak [minutes=60] [workers=8] [interval_s=30]` | `bench/soak.php` on a 256M arena: shifting Zipf working set, mixed sizes and TTLs, deletes, counters; one line per interval (hit %, percentiles, arena stats, worker RSS) |
| `./dev stampede [workers secs ttl compute_ms]` | `bench/stampede.php`: naive vs `apcu_entry` vs `nocopy_remember` |
| `./dev sfget [bundle src dir]` | Symfony bundle's `get()` against `AbstractAdapter`'s, then its PSR-6 reads (`getItem`, `hasItem`, `getItems`) against the bare `nocopy_get`, ns per call, best of 5 (`bench/sfget.php`; CLI, so copy mode and without Symfony's lock). Needs `.cache/sf-$PHP-8`, which `./dev symfony` leaves | `nocopy-linux:$PHP` |
| `./dev drupal-get [module dir]` | `NocopyBackend::get()` / `getMultiple()` against the bare `nocopy_get`, ns per call, best of 5 (`bench/drupal/get.php`; CLI, so copy mode). Needs the checkout `./dev drupal` leaves in `nocopy-drupal-test`; a module dir under `/src` (e.g. an older tree in `.cache/`) compares versions | `nocopy-drupal` |
| `./dev drupal-bench` | Drupal 11 under nginx + php-fpm + MariaDB, 9 cache configs, 4 pages (front page, node, term, JSON:API); results to `bench/results-drupal/`. Env: `WORKERS`, `CONC`, `REQS`, `REQS_AUTH`, `EDITOR_T`, `CONFIGS` (see `docker/drupal-bench.sh`); `MEM_ONLY=1` memory per worker, `ROWS_ONLY=1` (with `CONFIGS=stock`) the database cache rows after a crawl, put one by one into a fresh arena (`rows.tsv`), with `REPLAY="16M 24M"` also read through arenas of those sizes (`replay.tsv`; an old tree mounted at `/old` runs alongside, see `docker/drupal-bench.sh`), `PERF_ONLY=1` perf over the container (CPU by process, library, symbol); `NOCOPY_CFLAGS=-DNOCOPY_SHARE_STATS` with `ROWS_ONLY=1`: how much of what the stores hash is already shared (a `share` line on stderr) |
| `./dev cg [ops ...]` | cachegrind: instructions per op of `bench/prof.php` loops in copy mode (runs of `N` and `2N` ops, `cg_diff`), total and biggest functions per call (`OPS`); data in `.cache/perf` |
| `./dev perf [ops ...]` | perf profile of `bench/prof.php` hot loops (one op each), copy and zero mode; top symbols, `ANNOTATE="nocopy_fetch ..."` adds samples per source line; data in `.cache/perf` |
| `./dev trace [raw.csv ...]` | `docker/trace.sh`: Twitter cache traces (default `.cache/traces/*.csv`; download line in the script) replayed read-through with each key's TTL, nocopy (`bench/trace.php`, clock set to the trace's by libfaketime) vs libCacheSim's `cachesim` (v1.0.0) at equal bytes. Env: `SIZES` (MB, default `16 64 256`), `ALGOS` (default `lru,sieve,s3fifo,lecar,arc`), `N` (first N raw requests) |

Not wrapped (it needs extra mounts): `bench/objects.php` (object fetches vs `unserialize()`, and deepclone's payload if that extension is loaded);
usage in their headers and in [Develop, test and benchmark](../how-to/develop.md#benchmark).

## Tools

| Command | What it does |
|---|---|
| `./dev shell` | bash with nocopy built and loaded (`nocopy.shm_name=/nocopy-shell`), `/src` = the repository |
| `./dev stub` | regenerate `nocopy_arginfo.h` from `nocopy.stub.php` (writes to the checkout) |
| `./dev image` | build the image for `$PHP` |
| `./dev help` | list the commands |

## Files

| Path | Role |
|---|---|
| `docker/Dockerfile` | `nocopy-linux`: official `php:<PHP>-fpm` + nginx, Valkey, git, OpenSSL headers, clang + clang-tidy + compiler-rt, pcntl, APCu and igbinary (built, not loaded). PECL extensions in all Dockerfiles come as release tarballs from `pecl.php.net/get` pinned by `ADD --checksum` (no `pecl`, no PIE): bump version and sha256 together |
| `docker/Dockerfile.debug` | `nocopy-debug`: `nocopy-linux:<PHP>` + PHP from the image's own source with `--enable-debug` in `/opt/php-debug`, first in `PATH`, igbinary built for it |
| `docker/Dockerfile.zts` | `nocopy-zts`: `php:8.4-zts` + ext-parallel |
| `docker/Dockerfile.drupal` | `nocopy-drupal`: `nocopy-linux:8.5` + Drupal's extensions, MariaDB, composer |
| `docker/build.sh` | in-container build of `/src` into `/build/modules/nocopy.so` with `-Wall -Wextra -Werror` (`NOCOPY_CONFIGURE` for configure flags, `NOCOPY_ASAN=1` for sanitizers and `NOCOPY_DEBUG`) |
| `docker/lib.sh` | sourced by the in-container scripts: the module's path (`SO`), `nocopy_flags <name> [setting=value …]` (php flags for nocopy on the arena `/nocopy-<name>`), `shm_rm <name>`, `apcu_so`, `valkey_up`, `asan_env`, `run_tests` (`TIMEOUT` seconds) |
| `docker/*.sh` | the in-container scripts the commands above run |
