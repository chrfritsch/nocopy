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
| `./dev ci` (8.5, fetch mode from the SAPI, no `request_scope`; 2026-11-06, after objects moved to `nocopy_graph.c`, again after the layer seams were cleaned up (`nocopy_value.h`, `nocopy_shared`), again with hooks run between sizing passes (test 074); also `./dev drupal`; `./dev apcu` at 24 known with short value strings deduped) | all passed: tidy, test, novalkey, asan, clang, stress (30 loops), c, shm, zts, frankenphp, symfony |
| `./dev ci` (8.5; 2026-11-07, again with the keep cap and big entries left out of compaction (tests 076, 077); also `./dev drupal`) | all passed |
| `./dev ci` (8.5; 2026-11-07, dates stored natively (test 075), `./dev fuzz` added and in `ci`; also `./dev drupal`) | all passed, fuzz 20000 iterations clean in both modes |
| `./dev ci` (8.5; 2026-11-07, `nocopy_keys()` and `APCUIterator` in the polyfill (test 078)) | all passed |
| `./dev ci` (8.5; 2026-11-07, deduped value strings shared on copy-out, deletes on the cached hash, one borrow per `nocopy_get_multiple`) | all passed, fuzz 20000 iterations clean in both modes |
| `./dev test` (8.5, `./dev asan`, `./dev clang`; 2026-11-07) | 65 passed, 1 skipped (asan 66) |
| `./dev novalkey` | 46 passed, 12 skipped |
| `./dev zts` | 48 passed, 10 skipped |
| `./dev stress`, `./dev c` | clean (3 loops; CI runs 30) |
| `./dev shm`, `./dev frankenphp` | pass (php-fpm, FrankenPHP, with and without Valkey) |
| `./dev symfony` (6.4, 7.4, 8.x; 2026-11-06, with nocopy in front of the system pools) | pass |
| `./dev apcu` (2026-11-07, with `APCUIterator`) | 13 failures, all known |
| `./dev drupal` | 3 modes OK (zero-copy mode dropped: phpunit is CLI) |
| `./dev drupal-bench` | 9 configs, production PHP, JSON:API page; 6-config breakdown published 2026-11-06 evening, median of 3 ([results](../bench/results-drupal/2026-11-06-prod)) |
| `./dev opcache`, `./dev sfbench` with a `PhpFilesAdapter` pool (2026-11-07) | published ([results](reference/benchmarks.md#against-opcache-files-phpfilesadapter)): same req/s on read-only pages, nocopy reads 2.5-4x and writes 30-300x faster |
| `./dev tail` (2026-11-07, 8 and 4 workers, twice at 8) | published ([results](reference/benchmarks.md#tail-latency-dev-tail)); open: p99.99 under 8-way contention is 1-1.8 ms, level with APCu |
| `./dev sfbench` | published 2026-11-06 (evening, objects as graph nodes), with Symfony's production PHP settings ([results](reference/benchmarks.md#symfony-dev-sfbench)) |

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
