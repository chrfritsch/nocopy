# Install on a server

nocopy is not on PECL, PIE or in distribution packages yet: you build it from source.

**Requirements:** Linux (CI-tested on amd64 and arm64), PHP 8.4 or 8.5 (NTS or ZTS) with `phpize` (the `php-dev` /
`php-devel` package, or the official Docker images), a C compiler. For Valkey over TLS: OpenSSL 1.1.1+ headers and `pkg-config`.

## Build

```sh
git clone <this repository> nocopy && cd nocopy
phpize && ./configure && make
```

The extension is `modules/nocopy.so`. Check it loads:

```sh
php -d extension=$PWD/modules/nocopy.so -d nocopy.shm_name=/nocopy-try -r 'nocopy_put("a", [1, 2, 3]); var_dump(nocopy_get("a"));'
```

To build only the single-server cache (no Valkey tier, no OpenSSL, no `nocopy.valkey*` settings):

```sh
./configure --disable-nocopy-valkey
```

## Enable

Copy `modules/nocopy.so` to PHP's extension directory (`php-config --extension-dir`) or reference it by path, and add a
`conf.d` file that **both php-fpm and the CLI** read (they often use different directories, e.g. `/etc/php/8.5/fpm/conf.d`
and `/etc/php/8.5/cli/conf.d`):

```ini
extension=nocopy.so
nocopy.shm_name=/nocopy       ; required: the name of the shared cache
nocopy.shm_size=128M
```

Without `nocopy.shm_name` there is no cache at all: a startup warning, every fetch misses, every write returns `false`.
See [Share one cache between php-fpm and the CLI](share-between-fpm-and-cli.md) for the rules (same user, same build).

Restart php-fpm, then check from both sides:

```sh
php -r 'var_dump(nocopy_shared_with());'     # "host"
```

and in a web request, `nocopy_shared_with()` should also return `host`.

## In Docker

Containers get a 64 MB `/dev/shm` by default, and nocopy reserves its whole `nocopy.shm_size` there up front. Give the
container room:

```sh
docker run --shm-size=256m …
```

```yaml
# docker-compose.yml
services:
  php:
    shm_size: 256m
```

php-fpm and CLI commands (`docker exec`) in the **same** container share the cache. Separate containers do not: shared
memory and process ids are per container.

## Upgrade

1. Build and install the new `nocopy.so`.
2. Restart php-fpm (or FrankenPHP), and restart long-running CLI workers.

A new build uses a new shared-memory object (its name includes a hash of `nocopy.so`), so the cache starts empty. Old
objects are removed once no process uses them; a process that still finds one in use warns that those processes do not
share its cache.

## Uninstall

Remove the `conf.d` file, restart php-fpm, and remove leftovers: `rm /dev/shm/nocopy.*` (the name you configured, plus `.*`).
