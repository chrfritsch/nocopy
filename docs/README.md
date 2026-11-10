# nocopy documentation

nocopy is a shared-memory cache for PHP that hands out cached arrays without copying them.
These docs follow [Diátaxis](https://diataxis.fr): pick the column that matches what you need right now.

| | |
|---|---|
| **Tutorials**: learn by doing | **How-to guides**: get a task done |
| [Your first cache](tutorials/first-cache.md): build nocopy, cache 10,000 rows, read them from a web server in 17 ns | [Install on a server](how-to/install.md) |
| [Your first change](tutorials/first-change.md): change the C code, add a test, run what CI runs | [Share one cache between php-fpm and the CLI](how-to/share-between-fpm-and-cli.md) |
| | [Share a cache across servers with Valkey](how-to/valkey.md) |
| | [Use nocopy in Symfony](how-to/symfony.md) |
| | [Use nocopy in Drupal](../drupal/README.md) |
| | [Migrate from APCu](how-to/migrate-from-apcu.md) |
| | [Size and monitor the cache](how-to/size-and-monitor.md) |
| | [Develop, test and benchmark](how-to/develop.md) |
| **Reference**: look something up | **Explanation**: understand why |
| [Functions](reference/functions.md) | [How nocopy works](explanation/how-it-works.md) |
| [Configuration (php.ini)](reference/configuration.md) | [Sharing: by name and through Valkey](explanation/sharing.md) |
| [`nocopy_stats()`](reference/stats.md) | [Design decisions and rejected ideas](explanation/design-decisions.md) |
| [`./dev` commands](reference/dev-commands.md) | [Architecture and invariants](explanation/architecture.md) (for C changes) |
| [APCu compatibility](reference/apcu-compatibility.md) | [Correctness audit 2026-11: findings and plan](explanation/audit-2026-11.md) |
| [Benchmark results](reference/benchmarks.md) | [Design review 2026-11: premises attacked, findings ranked](explanation/design-review-2026-11.md) |
| | [Rebuilt from the start 2026-11: what would change, what would stay](explanation/rebuild-2026-11.md) |

Project state, what was verified last and what is planned: [Status and roadmap](status.md).
