# Status and roadmap

## State (2026-11-05)

Works, tested, **not released**. MIT licensed, free, no paid edition.

- **Platforms:** Linux, amd64 and arm64. Builds on macOS, untested there. No Windows.
- **PHP:** 8.4 and 8.5, NTS and ZTS (FrankenPHP; tested with ext-parallel, not with mod_php under a threaded MPM).
- **Pieces:** the extension (`nocopy_*`), [`apcu/`](../apcu) (polyfill, `chrfritsch/nocopy-apcu`),
  [`symfony/`](../symfony) (bundle, `cache.adapter.nocopy`), [`drupal/`](../drupal) (all bins + cache tags).
- **Distribution:** none yet. No GitHub remote, so CI ([`ci.yml`](../.github/workflows/ci.yml): PHP 8.4/8.5 × amd64/arm64,
  ZTS 8.4, FrankenPHP, Symfony 6.4/7.4/8) has only run locally through `./dev`.

## Last verified (2026-11-05, local Docker, arm64)

| Check | Result |
|---|---|
| `./dev ci` (8.5, fetch mode from the SAPI, no `request_scope`) | all passed: tidy, test, novalkey, asan, clang, stress (30 loops), c, shm, zts, frankenphp, symfony |
| `./dev test` (8.5) | 57 passed, 1 skipped |
| `./dev novalkey` | 46 passed, 12 skipped |
| `./dev zts` | 48 passed, 10 skipped |
| `./dev stress`, `./dev c` | clean (3 loops; CI runs 30) |
| `./dev shm`, `./dev frankenphp` | pass (php-fpm, FrankenPHP, with and without Valkey) |
| `./dev symfony` (8.x) | pass |
| `./dev apcu` | 23 failures, all known |
| `./dev drupal` | 3 modes OK (zero-copy mode dropped: phpunit is CLI) |
| `./dev drupal-bench` | 10 configs incl. `apcu-all` / `nocopy-all-dbtags` breakdown, published ([results](../bench/results-drupal/2026-11-05)) |
| `docker/fpm-bench.sh` | published; bench app fixed to use `cache.adapter.nocopy` (APCu layer gone since the polyfill) |

## Roadmap (rough priority)

1. **Release.** Push to GitHub and get CI green; `composer.json` and a GitHub release for PIE; split `drupal/`, `symfony/`,
   `apcu/` into their own repositories for Packagist.
2. **Numbers on real hardware:** a non-VM x86_64 box, `wrk` instead of `ab`. Micro benchmarks cross-checked on Graviton4
   (c8g.xlarge pod, 3 vCPU): ratios hold. Symfony and Drupal there are still to come.
3. **Eviction tuning:** victim choice is done (oldest + 3 least read of the 32 oldest, GL-Cache-inspired; see
   [Design decisions](explanation/design-decisions.md#eviction)). Open: when truly full, merge still copies data about to
   expire; skipping it needs a TTL workload in `bench/hitratio.php` first. 20% overwrites lost 0.9 points at 4M.
4. **Memory balance:** shards share a segment pool (done, see [Design decisions](explanation/design-decisions.md#memory-balance)).
   Open: a shard without writes expires only under memory pressure; RSS drifts to `nocopy.shm_size`. Steering segments
   by `ghost_hits` (Cliffhanger, CacheLib's MarginalHits) instead of an even share: only with a workload that needs it.
5. **Valkey:** Cluster (libvalkey's `cluster.c`, vendored next to the rest), Sentinel, TLS over unix sockets, CRL/OCSP, a `tls_server_name` override; an optional Valkey module
   to save round trips.

Want to help with one of these? Start with [Your first change](tutorials/first-change.md).
