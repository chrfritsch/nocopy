# Status and roadmap

## State (2026-11-08)

Works, tested, tagged **0.0.1** (first tag, no stability promise yet). MIT licensed, free, no paid edition.

- **Platforms:** Linux, amd64 and arm64. Builds on macOS, untested there. No Windows.
- **PHP:** 8.4 and 8.5, NTS and ZTS (FrankenPHP; tested with ext-parallel, not with mod_php under a threaded MPM).
- **Pieces:** the extension (`nocopy_*`), [`apcu/`](../apcu) (polyfill, `chrfritsch/nocopy-apcu`),
  [`symfony/`](../symfony) (bundle, `cache.adapter.nocopy`), [`drupal/`](../drupal) (all bins + cache tags), [`cli/`](../cli) (console, `chrfritsch/nocopy-cli`).
- **Distribution:** [github.com/chrfritsch/nocopy](https://github.com/chrfritsch/nocopy), tag 0.0.1, on [Packagist](https://packagist.org/packages/chrfritsch/nocopy):
  `pie install chrfritsch/nocopy` (verified on `php:8.5-cli`, 2026-11-08). The integrations are not on Packagist yet. CI ([`ci.yml`](../.github/workflows/ci.yml): PHP 8.4/8.5 × amd64/arm64,
  ZTS 8.4, FrankenPHP, Symfony 6.4/7.4/8) is green on GitHub Actions.

## Last verified (2026-11-09, local Docker, arm64)

| Check | Result |
|---|---|
| `./dev ci` (8.5; 2026-11-09, one wall clock (`nocopy_now`) and one monotonic (`nocopy_mono_ms`), one expiry formula (`nocopy_expiry`), one Valkey reply reader, `vk_cmd`, `GROW`; `./dev ab main 7` within noise; `fetch int, 100k distinct keys` swings 26-55 ns between runs of the same build) | all passed, stress 30 loops, fuzz 20000 iterations clean in both modes, apcu 13 known failures |
| `./dev ci` (8.5; 2026-11-09, cleanup: shared helpers (`set_cmd`, `push_is`, `seg_seal`, `open_fail`, `inl_grow`, `persist_new`), `nocopy_lookup`/`nocopy_remove`/`nocopy_keep_release` folded in, the unread `inserts` counter gone; `./dev ab main 7`: within noise) | all passed, stress 30 loops, fuzz 20000 iterations clean in both modes, apcu 13 known failures |
| `./dev ci` (8.5; 2026-11-09, slot holes (test 087), reference roots reset the key table (test 088), `pie` fetched once; also `./dev drupal`, its `testInvalidate` 10 runs clean) | all passed, stress 30 loops, fuzz 20000 iterations clean in both modes, apcu 13 known failures |
| `./dev ci` (8.5; 2026-11-09, short strings shared arena-wide (tests 034, 086, `tests/c/atoms.c`); `pie` rerun alone after a missing mount) | all passed, stress 30 loops, fuzz 20000 iterations clean in both modes, apcu 13 known failures |
| `./dev ci` (8.5, fetch mode from the SAPI, no `request_scope`; 2026-11-06, after objects moved to `nocopy_graph.c`, again after the layer seams were cleaned up (`nocopy_value.h`, `nocopy_shared`), again with hooks run between sizing passes (test 074); also `./dev drupal`; `./dev apcu` with short value strings deduped) | all passed: tidy, test, novalkey, asan, clang, stress (30 loops), c, shm, zts, frankenphp, symfony |
| `./dev ci` (8.5; 2026-11-07, again with the keep cap and big entries left out of compaction (tests 076, 077); also `./dev drupal`) | all passed |
| `./dev ci` (8.5; 2026-11-07, dates stored natively (test 075), `./dev fuzz` added and in `ci`; also `./dev drupal`) | all passed, fuzz 20000 iterations clean in both modes |
| `./dev ci` (8.5; 2026-11-07, `nocopy_keys()` and `APCUIterator` in the polyfill (test 078)) | all passed |
| `./dev ci` (8.5; 2026-11-07, deduped value strings shared on copy-out, deletes on the cached hash, one borrow per `nocopy_get_multiple`) | all passed, fuzz 20000 iterations clean in both modes |
| `./dev ci` (8.5; 2026-11-07, TTLs capped at 2^32 s (test 080), a too-small arena a startup warning (test 026)) | all passed |
| `./dev test` (8.5, `./dev asan`, `./dev clang`; 2026-11-07) | 65 passed, 1 skipped (asan 66) |
| `./dev novalkey` | 46 passed, 12 skipped |
| `./dev zts` | 48 passed, 10 skipped |
| `./dev stress`, `./dev c` | clean (3 loops; CI runs 30) |
| `./dev shm`, `./dev frankenphp` | pass (php-fpm, FrankenPHP, with and without Valkey; 2026-11-07: fpm workers create the cache as `www-data`, root CLI refused, a `nobody` pool with its own name) |
| `./dev symfony` (6.4, 7.4, 8.x; 2026-11-06, with nocopy in front of the system pools) | pass |
| `./dev apcu` (2026-11-07, with `APCUIterator`) | 13 failures, all known |
| `./dev drupal` | 3 modes OK (zero-copy mode dropped: phpunit is CLI) |
| `./dev drupal-bench` | 9 configs, production PHP, JSON:API page; 6-config breakdown published 2026-11-06 evening, median of 3 ([results](../bench/results-drupal/2026-11-06-prod)) |
| `./dev ci` (8.5; 2026-11-07, segments capped at 4 MB, bitmaps and segment table behind the slot tables) | all passed |
| `./dev big` (2026-11-07: 256M-8G, again 1G-4G with segments capped at 4 MB) | published ([results](reference/benchmarks.md#large-arenas-dev-big)): 4G now pauses writers like 1G (max 26-42 ms, was 51-111); reads unaffected |
| `./dev fpbench` (2026-11-07, twice) | published ([results](reference/benchmarks.md#frankenphp-worker-mode-dev-fpbench)): nocopy 87-94% of `hello` in worker mode, APCu 19-22% |
| `./dev soak` (2026-11-07: 50 min heavy churn, 30 + 10 min TTLs with a read-only stretch) | published ([results](reference/benchmarks.md#soak-dev-soak)): flat, no leak; expired entries stay until the next write |
| `./dev tail` with up to 1,000 idle slot holders, `./dev sfbench` with 256 fpm workers (2026-11-07) | published ([results](reference/benchmarks.md#many-workers)): no scaling cost |
| `./dev opcache`, `./dev sfbench` with a `PhpFilesAdapter` pool (2026-11-07) | published ([results](reference/benchmarks.md#against-opcache-files-phpfilesadapter)): same req/s on read-only pages, nocopy reads 2.5-4x and writes 30-300x faster |
| `./dev tail` (2026-11-07, 8 and 4 workers, twice at 8) | published ([results](reference/benchmarks.md#tail-latency-dev-tail)); open: p99.99 under 8-way contention is 1-1.8 ms, level with APCu |
| `./dev sfbench` | published 2026-11-06 (evening, objects as graph nodes), with Symfony's production PHP settings ([results](reference/benchmarks.md#symfony-dev-sfbench)) |

## Roadmap (rough priority)

1. **Release.** The extension is on Packagist (done); split `drupal/`, `symfony/`,
   `apcu/`, `cli/` into their own repositories for Packagist.
2. **Numbers on real hardware:** a non-VM x86_64 box, `wrk` instead of `ab`. Micro benchmarks cross-checked on Graviton4
   (c8g.xlarge pod, 3 vCPU): ratios hold. Symfony and Drupal there are still to come.
3. **Eviction tuning:** large arenas done: segments stop at 4 MB, so an eviction round stalls a shard's writers no longer at 4G than at 1G ([design decisions](explanation/design-decisions.md#eviction)). Also victim choice is done (oldest + 3 least read of the 32 oldest, GL-Cache-inspired; see
   [Design decisions](explanation/design-decisions.md#eviction)). Open: when truly full, merge still copies data about to
   expire; skipping it needs a TTL workload: `./dev trace` (Twitter traces with TTLs, against libCacheSim's policies) is set up, not run yet. 20% overwrites lost 0.9 points at 4M.
4. **Arena size:** strings up to 64 bytes are stored once arena-wide (done, 2026-11-09): a warmed Drupal's 6,477 database cache rows take 16% less (46.2 -> 38.7 MB), and read through a 32M arena they hit 94.6% instead of 91.9%; fetches unchanged, array stores 4-6% slower
   ([design decisions](explanation/design-decisions.md#writes)). Open: compression (LZ4/zstd on entries costs every hit; on Valkey payloads it pays only when Valkey is short of memory or bandwidth).
   Found while measuring: `./dev drupal-bench` ran without a cache since fpm workers create it as `www-data` (Drush, as root, created it first; fixed: Drush runs as `www-data`). With the cache back, `nocopy-all` crashed php-fpm workers in some runs (1 of 3: `zend_mm_heap corrupted`, then SIGSEGV in every worker at the editor phase's start). Cause, fixed 2026-11-09: a value that is a reference (the Drupal backend's `invalidateMultiple()` passes `foreach (... as &$item)` elements to `nocopy_put_multiple()`) kept the last value's key table, so its strings could point into another entry or be persisted short of what sizing reserved (test 088; the module's `testInvalidate` failed 9-10 of 10 with shared strings, 0 of 10 after).
5. **Memory balance:** shards share a segment pool (done, see [Design decisions](explanation/design-decisions.md#memory-balance)).
   Open: a shard without writes expires only under memory pressure (soak: entries and `used` stay frozen, harmless since the memory is reserved up front); RSS drifts to `nocopy.shm_size`. Steering segments
   by `ghost_hits` (Cliffhanger, CacheLib's MarginalHits) instead of an even share: only with a workload that needs it.
6. **Valkey:** Cluster (libvalkey's `cluster.c`, vendored next to the rest), Sentinel, TLS over unix sockets, CRL/OCSP, a `tls_server_name` override; an optional Valkey module
   to save round trips.

Want to help with one of these? Start with [Your first change](tutorials/first-change.md).
