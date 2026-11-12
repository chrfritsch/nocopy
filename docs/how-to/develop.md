# Develop, test and benchmark

Everything runs in Docker through `./dev`; you need no PHP, compiler or Valkey on your machine. CI runs the same commands.
New here? Do the [first change tutorial](../tutorials/first-change.md) first. All commands: [`./dev` reference](../reference/dev-commands.md).

## Run tests

```sh
./dev test                                   # full .phpt suite, PHP 8.5
./dev test tests/018_shift_under_readers.phpt
PHP=8.4 ./dev test                           # another PHP version
```

Each run copies the sources into the container and builds from clean, so stale objects cannot fool you and your checkout
stays untouched (it is mounted read-only). A failed test prints its diff: `-` expected, `+` actual.

## Pick the checks for your change

| You changed | Run |
|---|---|
| anything in C | `./dev test`, `./dev novalkey` (the core must build without the Valkey tier) |
| locking, pins, reclamation, slot tables, eviction | also `./dev stress 30` and `./dev c 5` |
| `nocopy_valkey.c` | also `./dev shm` (php-fpm + FrankenPHP with Valkey, listener restarts) |
| `third_party/libvalkey` (only via `scripts/vendor-libvalkey.sh <tag>`, never by hand) | `./dev ci` |
| thread handling, FrankenPHP | `./dev zts`, `./dev frankenphp` |
| `nocopy.stub.php` (function signatures) | `./dev stub` to regenerate `nocopy_arginfo.h`, commit both |
| `apcu/apcu.php` | `./dev apcu` (13 known failures, listed in `scripts/apcu-suite.sh`) |
| `symfony/` | `./dev symfony`, and `./dev symfony 6.4.*` for the oldest supported |
| `drupal/` | `./dev drupal` (PHPUnit in 4 cache modes) |
| before a pull request | `./dev ci` |

A single run of a concurrency test catches a race only sometimes: that is what the loops are for. Read the invariants in
[Architecture](../internals/architecture.md) before touching locking or the read path.

## Write a test

Add `tests/0NN_what_it_checks.phpt` (next free number). Rules:

- Its own `nocopy.shm_name=/nocopy-tNNN` in `--INI--`, and `--CLEAN--` removing `/dev/shm/nocopy-tNNN*`: tests run in parallel.
- Tests run under the CLI, so they fetch copies. For zero-copy, run the body as a `php -S` request: `require __DIR__ . '/serve.inc'; echo serve(<<<'CODE' ... CODE);` (forwards the test's `nocopy.*` INI; 004 is an example).
- Tests that need memory pressure use a small `nocopy.shm_size` (2M-4M, one shard).
- Valkey tests require `valkey_skip.inc` in `--SKIPIF--` (they skip without a server) and set `nocopy.valkey=127.0.0.1:6379`
  with their own `nocopy.valkey_prefix=tNNN:`; `valkey.inc` has helpers. Add `--CONFLICTS--` `valkey`: they share one server, so they run one at a time.
- Forked processes: `pcntl_fork()`; threads (ZTS only): ext-parallel. Another PHP process with this build (its own arena, another user, other settings):
  `require_once __DIR__ . '/php.inc'`, then `php_run($code, ['nocopy.shm_name' => '/nocopy-tNNN-other'])`, or `php_cmd()` / `php_argv()` for `exec` and `proc_open`.

## Try things by hand

```sh
./dev shell
```

Bash in the image with your current sources built and loaded, `/src` = the repository, APCu and igbinary available but not
loaded. `php -a` for an interactive PHP shell.

## Benchmark

Claims about speed need numbers. On a quiet machine:

```sh
./dev bench          # micro benchmarks: APCu vs nocopy copy vs nocopy zero-copy, medians of 3
./dev stampede       # concurrent recomputation of an expiring key
```

To compare two versions of the code (e.g. `main` and your change), run both trees interleaved in one container so noise
hits both alike:

```sh
./dev ab main 3 "read write"                   # main vs the working tree, best of 3
ONLY='store array' ./dev ab HEAD~1 7 write     # only the measurements you changed: seconds per round, not minutes
```

A full round (all groups, both trees, both modes) takes about 2 minutes; `read` and `write` are most of it (0.65 s per measurement).

Hit ratio under a key distribution, in `./dev shell`:

```sh
php -d nocopy.shm_name=/hr -d nocopy.shm_size=4M bench/hitratio.php zipf 1.5 0
```

Full-stack: `./dev drupal-bench` (Drupal 11 behind nginx + php-fpm + MariaDB, 9 cache configs, long; writes to
`bench/results-drupal/`), and `./dev sfbench` for a Symfony app.

Record what you decided and why, with the numbers, in [Design decisions](../internals/design-decisions.md).

## Without Docker

On Linux with PHP dev headers: `phpize && ./configure && make && make test TESTS=tests`. A debug build of PHP catches more.
The arena alone: `tests/c/run.sh` (needs a C compiler with ThreadSanitizer).

## Gotchas

- Docker's `/dev/shm` is 64 MB unless you pass `--shm-size` (Docker Desktop gives more, so a local pass can hide it); every `./dev` container gets 1 GB, benches more where they need it.
- A container started right after you saved a file on macOS sometimes sees a truncated source file: rerun.
- Don't edit `./dev` while a `./dev` command runs: the shell reads the script as it goes.
