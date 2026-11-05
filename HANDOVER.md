# Handover: nocopy

`README.md` = usage and headline numbers. `ARCHITECTURE.md` = how it works and its invariants. This file = state, how to run things,
decisions with the numbers behind them, and what is open. History is in `git log`.

## State (2026-11-05)

Works, not released, MIT. No remote yet, so CI (`.github/workflows/ci.yml`: PHP 8.4/8.5 x amd64/arm64, ZTS 8.4, FrankenPHP, Symfony 6.4/7.4/8)
has never run on GitHub. Linux only (macOS builds; untested there). Pieces: the extension (`nocopy_*`, `nocopy.shm_name` required),
`apcu/` (PHP polyfill, `chrfritsch/nocopy-apcu`), `symfony/` (bundle, `cache.adapter.nocopy`), `drupal/` (all bins + cache tags).

Last verified (2026-11-05): suite 8.4/8.5/ZTS 8.4, tests/c with TSAN, `stress.sh 5`, `shm-name.sh` (fpm, FrankenPHP, Valkey), FrankenPHP, Symfony 8 smoke.
Drupal tests in 4 modes pass; `docker/drupal-bench.sh` (one run, noisy host: stock's logged-in `/` 20-25% under its range) matches the README:
nocopy-all anonymous `/` 12,229 req/s, 7 statements per logged-in request, first request after an fpm restart 146 ms. Committed results not replaced.

## Run things (Docker; `docker build --build-arg PHP=8.5 -t nocopy-linux:8.5 docker`)

| What | Command |
|---|---|
| suite | `docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/test.sh [tests/0xx.phpt]` |
| race loop / arena from threads | `docker/stress.sh 30` / `tests/c/run.sh 5` (same image) |
| shared memory + Valkey, fpm and FrankenPHP | `sh /src/docker/shm-name.sh` (same image; FrankenPHP: `dunglas/frankenphp:1-php8.4`) |
| micro benchmarks vs APCu, A/B of two trees | `docker run --rm --shm-size=1g ... /src/docker/bench.sh`, `/src/docker/ab.sh [reps] [groups]` |
| Symfony app behind fpm | `docker/fpm-bench.sh` (mount an app with vendor at `/appsrc`; `bench/sfapp` holds src/public/composer.json) |
| stampede | `docker/stampede.sh` |
| hit ratio | `php -d extension=… -d nocopy.shm_name=/hr -d nocopy.shm_size=4M bench/hitratio.php zipf 1.5 0` |
| Drupal tests / benchmark | `docker/drupal-test.sh`, `docker/drupal-bench.sh` (image `docker/Dockerfile.drupal`) |
| APCu's suite on the polyfill | `PHP=… NOCOPY_SO=… scripts/apcu-suite.sh` (22 known failures, listed in the script) |

Gotchas: tests and scripts give every arena its own name and remove `/dev/shm/<name>.*` afterwards; Docker's `/dev/shm` is 64M unless
`--shm-size`. After changing sources, rebuild from clean (stale `.lo` fooled us twice). A container started right after a host-side write
sometimes saw a truncated `nocopy.c`: rerun.

## Decisions and their numbers

- Zero-copy lifetime: per-worker pin bitmaps over segments (an epoch + per-entry retire list piled up garbage; old "zero-copy write 50-100x slower" results came from it).
- `nocopy.shm_name` required, unset or not attachable = no cache + startup warning, never a private one (a private cache splits invalidation silently).
  No default name: two sites of one user would share keys.
- APCu compatibility is a PHP polyfill; the extension keeps only `apc.enabled`/`apc.enable_cli` (Symfony's `ApcuAdapter::isSupported()` and Composer check them).
  `bench/run.php` measures `nocopy_*` directly by re-running itself with `apcu_*` rewritten (fetch int 11.7 ns zero-copy, not the polyfill's extra call).
- Slot home from a mixed hash: raw DJBX33A clustered `prefix.N` keys (one shard at 73% fill: hit 28.8 -> 17.8 ns, miss 58.3 -> 15.2, delete+put 107 -> 29);
  price: `fetch int, 100k distinct keys` 22.1 -> 26.8 ns (keys spread over more cache lines). Test 018 picks keys homed in the first half to keep its race window.
- Kept copies in copy mode (`nocopy.local_size` 8M): 1k rows 68 us -> 12.8 ns, 10k rows 711 us -> 12.8 ns, string 10 KB 182 -> 12.8 ns; small values <= 0.8 ns slower.
  Kept on the second fetch only (doorkeeper): keeping every copy made 100k distinct strings 37 -> 64 ns. Over budget it flushes all (ponytail ceiling).
- Leases in a fixed per-shard table, not in the cache (they used slots, showed up in counts, were wiped by clear, and a drop could hit a retaken lease).
- Valkey: one listener process per arena holds the only tracking connection (16 connections and 16 copies of every push per server became 1);
  the writer's own server refetches its write once (the listener cannot tell own writes apart).
- No per-entry hit counters (a shared write per read). Slot tables fixed, not growable (no stall; price: tiny entries evict by count, raise `nocopy.entries_hint`).
- No `nocopy.ttl` default: it expired Drupal's cache tag tokens. Callers pass `$ttl`.
- Valkey tier behind `nocopy_vk_*`, compiled out with `--disable-nocopy-valkey` (CI runs the suite both ways): the core builds and ships without it; default stays on.
- Code comments: one short line (user preference).

## Measured and rejected

- `MADV_HUGEPAGE`: acts only with `shmem_enabled=advise` (default never); then 100k distinct keys -3.6/-6.6%, parallel fetches 2-4% slower.
- Asymmetric fence (`membarrier`) for pins: reader fence hides under cache misses (shuffled 40.9 -> 41.3 ns); writer pays 6-46 us under the shard lock with 4-16 processes.
- Robust pthread mutexes for dead lock holders: 15% slower uncontended, 2-8x with contention (owner-id locks + `shard_repair` instead).
- Ghost variants (`bench/hitratio.php`): 2-way no better; 2x tags +0-3 points on mixed loads for 4 B/slot more; no ghost loses 2 points on mixed and all hits on scans.
- Dropping `nocopy_get_multiple`/`put_multiple` on one host (PHP loop instead): get 10 keys 142 -> 292 ns, 100 keys 1.3 -> 2.8 us; put 100 keys 8.3 -> 9.3 us.
  `nocopy_shared_with()` into `nocopy_stats()`: 15 -> 430 ns, and Drupal calls it per write.
- Dropping `nocopy.entries_hint` for 1 slot per 128 B (was 256): `fetch int, 100k distinct keys` 30.3 -> 39.2 ns (-22%), 500 B values -0.6/-1.1 hit points
  (4.7% of the arena more in tables); only tiny entries gain (+7-9 points). Dropping the knob alone keeps the defaults but loses that +7-9 for who sets it.
- One-pass `copy_out`: `zend_array_dup` of an immutable array is one memcpy already; the cost was per-key allocation (fixed by key dedupe).

## Open (rough priority)

1. Push to GitHub, get CI green; `composer.json` + a GitHub release for PIE; subtree split for `drupal/`, `symfony/`, `apcu/` on Packagist.
2. Numbers on a real (non-VM) x86_64 box, wrk instead of ab.
3. Merge keeps read entries up to one segment per 4 merged (test 029): tune if full-cache hit rates need it. When truly full, evacuation copies data about to expire.
4. A shard without writes expires only under memory pressure; a hot shard cannot borrow from idle ones; RSS drifts to `nocopy.shm_size`.
5. Valkey: Sentinel, Cluster, TLS over unix sockets, CRL/OCSP, `tls_server_name` override; an optional Valkey module to save round trips.
