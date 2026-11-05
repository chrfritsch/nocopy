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
| `./dev test` (8.5) | 54 passed, 1 skipped |
| `./dev novalkey` | 44 passed, 11 skipped |
| `./dev zts` | 46 passed, 9 skipped |
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
3. **Eviction tuning:** merge keeps read entries up to one segment per 4 merged (test 029): tune if full-cache hit rates
   need it. When truly full, evacuation copies data about to expire.
4. **Memory balance:** a shard without writes expires only under memory pressure; a hot shard cannot borrow from idle ones;
   RSS drifts to `nocopy.shm_size`.
5. **Valkey:** Sentinel, Cluster, TLS over unix sockets, CRL/OCSP, a `tls_server_name` override; an optional Valkey module
   to save round trips.

Want to help with one of these? Start with [Your first change](tutorials/first-change.md).
