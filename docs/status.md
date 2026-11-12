# Status and roadmap

## Current release: 0.0.1

The first release. nocopy works and is well tested, but there is **no stability promise yet**: function signatures,
settings and integration APIs may still change between releases. Read the release notes before you upgrade.

Install with `pie install chrfritsch/nocopy` ([install guide](how-to/install.md)). MIT licensed, free, no paid edition.

## What is supported

| | |
|---|---|
| **Operating system** | Linux on amd64 and arm64. It builds on macOS, but that is untested. No Windows. |
| **PHP** | 8.4 and 8.5, NTS and ZTS (thread-safe) |
| **Web servers** | php-fpm, Apache mod_php (prefork), FrankenPHP (classic and worker mode), `php -S`, CGI, LiteSpeed. Not tested: mod_php under a threaded Apache MPM. |
| **Long-running processes** | CLI scripts, queue workers, FrankenPHP workers (they get [copy mode](explanation/how-it-works.md#zero-copy-and-copy-mode) automatically) |
| **Several servers** | Valkey, or Redis 6.2+, including over TLS. Not yet: Sentinel, Cluster, TLS over unix sockets. |

### Integrations

| Package | For | Status |
|---|---|---|
| `chrfritsch/nocopy-apcu` | `apcu_*` functions for code you can't change | works; not on Packagist yet ([install](how-to/migrate-from-apcu.md#route-b-the-polyfill-no-code-changes)) |
| `chrfritsch/nocopy-bundle` | Symfony 6.4, 7.x and 8.x | works; not on Packagist yet ([install](how-to/symfony.md)) |
| `chrfritsch/nocopy-drupal` | Drupal 11.2+ | works; not on Packagist yet ([install](../drupal/README.md)) |
| `chrfritsch/nocopy-cli` | console: stats, keys, get, delete, clear | works; not on Packagist yet |

Until they are on Packagist, require them from a checkout of this repository, as each guide shows.

## How it is tested

GitHub Actions runs these on every push:

- **The test suite** on PHP 8.4 and 8.5, amd64 and arm64, thread-safe PHP, php-fpm and FrankenPHP, and a build without
  Valkey support.
- **Memory and thread checkers:** AddressSanitizer, ThreadSanitizer, a debug build of PHP, and leak checks.
- **Concurrency:** race tests repeated 30 times in a row, and a tool that tries every order in which concurrent
  operations can interleave.
- **Crashes:** writers killed with `kill -9` in the middle of a write; the cache must repair itself.
- **Fuzzing:** 20,000 random values per run, stored and read back.
- **Integrations:** a Symfony app on 6.4, 7.4 and 8, and APCu's own test suite against the polyfill (13 known
  differences, [listed here](reference/apcu-compatibility.md#apcus-test-suite)).

Run by hand before changes to them: the Drupal module's tests, including Drupal core's generic cache backend tests
(on one server, with Valkey, and without a shared cache), and a 50-minute soak test under heavy churn, which shows no leak.

## Known limitations

- **Benchmarks** come from Docker on an Apple M3 Pro, cross-checked on AWS Graviton4. Numbers on dedicated x86_64 hardware
  are still to come ([benchmarks](reference/benchmarks.md)).
- **Expired entries** are removed during writes. A cache that only gets reads keeps them (they are never returned) until
  the next write. This does not cost memory, since the whole size is reserved up front anyway.
- **Valkey:** each write makes every server fetch the value again once, so Valkey is not the place for a counter you bump on
  every request ([details](how-to/valkey.md#good-to-know)).

## Roadmap

In rough order of priority:

1. **Packagist:** publish the APCu polyfill, the Symfony bundle, the Drupal module and the console as their own packages.
2. **Numbers on real hardware:** Symfony and Drupal benchmarks on a dedicated x86_64 server.
3. **Smarter eviction when full:** skip copying entries that are about to expire anyway.
4. **Smaller cache entries:** compression is being considered, but only where it does not slow down reads.
5. **Valkey:** Cluster, Sentinel, TLS over unix sockets, certificate revocation checks.

## Get involved

Found a bug? Open an issue with your PHP version, how PHP runs (php-fpm, FrankenPHP, CLI…), `php -v` (it shows `ZTS` or
`NTS`) and the output of `nocopy_stats()`. Want to help with the roadmap? Start with
[Your first change](tutorials/first-change.md). Contributors find the detailed verification log and roadmap in
[the internals](internals/status.md).
