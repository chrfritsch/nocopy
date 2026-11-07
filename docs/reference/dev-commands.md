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
| `./dev asan [tests …]` | same, built with AddressSanitizer + UBSan (libasan preloaded into the uninstrumented php, `USE_ZEND_ALLOC=0`, no leak check) | `nocopy-linux:$PHP` |
| `./dev clang [tests …]` | same, built with clang (warnings gcc does not give; `-Werror` too) | `nocopy-linux:$PHP` |
| `./dev zts [tests …]` | suite on thread-safe PHP 8.4 with ext-parallel | `nocopy-zts:8.4` |
| `./dev stress [loops=30]` | concurrency tests (011, 015, 018, 021, 022) in a loop on a release build, stops at the first failure or hang | `nocopy-linux:$PHP` |
| `./dev c [seconds=3]` | the arena alone (`tests/c/`) from 8 threads, plain and ThreadSanitizer, then the repair test | `nocopy-linux:$PHP` |
| `./dev fuzz [iterations=20000] [seed]` | `tests/fuzz.php` on an ASan + UBSan build, copy mode then zero-copy: random values (every object kind, dates, shared references, cycles, deep nesting) stored over 1024 keys in a 4 MB arena, so merges move them and evict; every fetch must `serialize()` as `unserialize(serialize())` of what was stored. Prints the seed; the same iterations and seed replay the run, `FUZZ_VALUE=<iteration>` dumps the value one iteration made | `nocopy-linux:$PHP` |
| `./dev shm` | CLI and php-fpm share one cache by name, survive reloads and restarts, churn without misses; then the same with Valkey | `nocopy-linux:$PHP` |
| `./dev frankenphp` | FrankenPHP classic and worker mode; then the `shm` checks under FrankenPHP | `dunglas/frankenphp:1-php8.4` |
| `./dev symfony [version=8.*]` | Symfony bundle smoke test, alone and next to APCu; installs `symfony/framework-bundle` into `.cache/` | `nocopy-linux:$PHP` |
| `./dev apcu` | APCu's own test suite against the polyfill (24 known failures) | `nocopy-linux:$PHP` |
| `./dev drupal` | Drupal module PHPUnit tests in 3 modes: shared (CLI: copy mode), Valkey, no shared cache. Drupal checkout kept in volume `nocopy-drupal-test` | `nocopy-drupal` |
| `./dev tidy` | clang-tidy over the extension's C sources, checks in `.clang-tidy`; any finding fails | `nocopy-linux:$PHP` |
| `./dev ci` | `tidy test novalkey asan clang stress c fuzz shm zts frankenphp symfony`, stops at the first failure | |

## Benchmarks

| Command | What it runs |
|---|---|
| `./dev bench [groups]` | `bench/run.php` groups (default all: read write parallel mixed contend ttl) ×3 for APCu, nocopy copy and zero-copy; prints medians. Env: `ONLY` (regex on measurement names, the rest skipped) |
| `./dev ab [ref=main] [reps=3] [groups]` | `docker/ab.sh`: `ref` (via `git archive`, in `.cache/ab-old`) vs the working tree, interleaved in one container, best of `reps`, copy and zero mode. Env: `ONLY` |
| `./dev sfbench` | `bench/sfapp` under nginx + php-fpm with Symfony's recommended production PHP settings: APCu (with and without Symfony's marshaller) vs the bundle, 4 pages, median of 3. Env: `WORKERS`, `REQS`, `CONC` |
| `./dev stampede [workers secs ttl compute_ms]` | `bench/stampede.php`: naive vs `apcu_entry` vs `nocopy_remember` |
| `./dev drupal-bench` | Drupal 11 under nginx + php-fpm + MariaDB, 9 cache configs, 4 pages (front page, node, term, JSON:API); results to `bench/results-drupal/`. Env: `WORKERS`, `CONC`, `REQS`, `REQS_AUTH`, `EDITOR_T`, `CONFIGS` (see `docker/drupal-bench.sh`); `MEM_ONLY=1` memory per worker, `PERF_ONLY=1` perf over the container (CPU by process, library, symbol) |
| `./dev perf [ops ...]` | perf profile of `bench/prof.php` hot loops (one op each), copy and zero mode; top symbols, `ANNOTATE="nocopy_fetch ..."` adds samples per source line; data in `.cache/perf` |

Not wrapped (they need extra mounts): `bench/sfget.php` (Symfony `get()`, bundle vs `AbstractAdapter`) and `bench/objects.php` (object fetches vs `unserialize()`, and deepclone's payload if that extension is loaded);
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
| `docker/Dockerfile` | `nocopy-linux`: official `php:<PHP>-fpm` + nginx, Valkey, git, OpenSSL headers, clang + clang-tidy + compiler-rt, pcntl, APCu and igbinary (built, not loaded) |
| `docker/Dockerfile.zts` | `nocopy-zts`: `php:8.4-zts` + ext-parallel |
| `docker/Dockerfile.drupal` | `nocopy-drupal`: `nocopy-linux:8.5` + Drupal's extensions, MariaDB, composer |
| `docker/build.sh` | in-container build of `/src` into `/build/modules/nocopy.so` with `-Wall -Wextra -Werror` (`NOCOPY_CONFIGURE` for configure flags, `NOCOPY_ASAN=1` for sanitizers) |
| `docker/*.sh` | the in-container scripts the commands above run |
