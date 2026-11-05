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
| `./dev test` (8.5) | 55 passed, 1 skipped |
| `./dev novalkey` | 45 passed, 11 skipped |
| `./dev zts` | 47 passed, 9 skipped |
| `./dev stress`, `./dev c` | clean (3 loops; CI runs 30) |
| `./dev shm`, `./dev frankenphp` | pass (php-fpm, FrankenPHP, with and without Valkey) |
| `./dev symfony` (8.x) | pass |
| `./dev apcu` | 23 failures, all known |
| `./dev drupal` | 4 modes OK |
| `./dev drupal-bench` | matches the published numbers (nocopy, all bins: anonymous 12,229 req/s, 7 statements logged-in, first request 146 ms) |

## Roadmap (rough priority)

1. **Release.** Push to GitHub and get CI green; `composer.json` and a GitHub release for PIE; split `drupal/`, `symfony/`,
   `apcu/` into their own repositories for Packagist.
2. **Numbers on real hardware:** a non-VM x86_64 box, `wrk` instead of `ab`.
3. **Eviction tuning:** victim choice is done (oldest + 3 least read of the 32 oldest, GL-Cache-inspired; see
   [Design decisions](explanation/design-decisions.md#eviction)). Open: when truly full, merge still copies data about to
   expire; skipping it needs a TTL workload in `bench/hitratio.php` first. 20% overwrites lost 0.9 points at 4M.
4. **Memory balance:** a shard without writes expires only under memory pressure; a hot shard cannot borrow from idle ones;
   RSS drifts to `nocopy.shm_size`. Candidate: move segments toward the shard with the most `ghost_hits` (Cliffhanger,
   CacheLib's MarginalHits), only once a benchmark shows shards out of balance; it breaks the fixed-slice invariant.
   Cheaper first: document `ghost_hits` as the "cache too small" signal in [Size and monitor](how-to/size-and-monitor.md).
6. **Copy mode:** the kept-copy table is direct-mapped (4096 slots), so colliding keys evict each other (a 3000-key loop
   fetches at ~400 ns with room to spare); set-associative if that shows up in workers.
5. **Valkey:** Sentinel, Cluster, TLS over unix sockets, CRL/OCSP, a `tls_server_name` override; an optional Valkey module
   to save round trips.

Want to help with one of these? Start with [Your first change](tutorials/first-change.md).
