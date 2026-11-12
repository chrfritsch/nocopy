# nocopy documentation

nocopy is a shared-memory cache for PHP, like APCu, that hands out cached arrays without copying them. php-fpm, the CLI
and cron share one cache, and with Valkey, several servers do.

## Where to start

- **New to nocopy?** Do the [first cache tutorial](tutorials/first-cache.md) (10 minutes, only Docker needed), then
  [install it on a server](how-to/install.md).
- **Coming from APCu?** Read [Migrate from APCu](how-to/migrate-from-apcu.md).
- **Using Symfony or Drupal?** Go to the [Symfony bundle](how-to/symfony.md) or the [Drupal module](../drupal/README.md).
- **Looking up a function or setting?** See [Functions](reference/functions.md) and [Configuration](reference/configuration.md).
- **Want to change the C code?** Start with [Your first change](tutorials/first-change.md).

## All pages

The docs are split by what you need right now ([Diátaxis](https://diataxis.fr)).

| | |
|---|---|
| **Tutorials**: learn by doing | **How-to guides**: get a task done |
| [Your first cache](tutorials/first-cache.md): build nocopy, cache 10,000 rows, read them from a web server in nanoseconds | [Install on a server](how-to/install.md) |
| [Your first change](tutorials/first-change.md): change the C code, add a test, run what CI runs | [Share one cache between php-fpm and the CLI](how-to/share-between-fpm-and-cli.md) |
| | [Share a cache across servers with Valkey](how-to/valkey.md) |
| | [Run nocopy on Kubernetes](how-to/kubernetes.md) |
| | [Use nocopy in Symfony](how-to/symfony.md) |
| | [Use nocopy in Drupal](../drupal/README.md) |
| | [Migrate from APCu](how-to/migrate-from-apcu.md) |
| | [Size and monitor the cache](how-to/size-and-monitor.md) |
| | [Develop, test and benchmark](how-to/develop.md) (contributors) |
| **Reference**: look something up | **Explanation**: understand why |
| [Functions](reference/functions.md) | [How nocopy works](explanation/how-it-works.md) |
| [Configuration (php.ini)](reference/configuration.md) | [Sharing: by name and through Valkey](explanation/sharing.md) |
| [`nocopy_stats()`](reference/stats.md) | [Architecture](explanation/architecture.md) |
| [Glossary](reference/glossary.md) | [Design decisions](explanation/design-decisions.md) |
| [APCu compatibility](reference/apcu-compatibility.md) | |
| [Benchmarks](reference/benchmarks.md) | |
| [`./dev` commands](reference/dev-commands.md) (contributors) | |

## For contributors

Changing the C code? Read these first (they are written for contributors and their tools, not for users):

- [Internals](internals/architecture.md): code layers, memory layout, locking, eviction, and the invariants every change must keep
- [Design log](internals/design-decisions.md): every decision with its measurements, and the ideas that were tried and dropped
- [Status log](internals/status.md): what was verified when, and the open work in detail
- [Benchmark log](internals/benchmarks.md): every benchmark run in full, with setup and caveats

Release state, what is supported and what is planned: [Status and roadmap](status.md).
