# Install on a server

This guide installs the extension, turns it on for php-fpm and the CLI, and checks that both share one cache.
nocopy installs with [PIE](https://github.com/php/pie) or from source; it is not on PECL or in distribution packages.

**You need:**

- Linux (tested on amd64 and arm64)
- PHP 8.4 or 8.5, NTS or ZTS, with `phpize`: the `php-dev` / `php-devel` package, or one of the official Docker images
- A C compiler
- For Valkey over TLS: OpenSSL 1.1.1+ headers and `pkg-config`

## 1. Build and install

### With PIE

```sh
pie install chrfritsch/nocopy
```

PIE builds the extension, copies `nocopy.so` to PHP's extension directory and adds `extension=nocopy` to the `php.ini` of
the PHP it runs with. It needs `libtool` and `unzip`; `--auto-install-build-tools` installs them for you.

Options:

```sh
pie install chrfritsch/nocopy --disable-nocopy-valkey   # one server only: no Valkey support, no OpenSSL needed
pie install chrfritsch/nocopy --disable-nocopy-lto      # without link-time optimization
```

In a Dockerfile, on the official PHP images:

```dockerfile
COPY --from=ghcr.io/php/pie:bin /pie /usr/bin/pie
RUN apt-get update && apt-get install -y --no-install-recommends git libtool unzip libssl-dev pkg-config \
 && pie install chrfritsch/nocopy \
 && printf 'nocopy.shm_name=/nocopy\nnocopy.shm_size=128M\n' > /usr/local/etc/php/conf.d/nocopy.ini
```

### From source

```sh
git clone https://github.com/chrfritsch/nocopy && cd nocopy
phpize && ./configure && make      # ./configure --disable-nocopy-valkey for one server only
```

The extension is `modules/nocopy.so`. Check that it loads:

```sh
php -d extension=$PWD/modules/nocopy.so -d nocopy.shm_name=/nocopy-try -r 'nocopy_put("a", [1, 2, 3]); var_dump(nocopy_get("a"));'
```

Then copy `modules/nocopy.so` to PHP's extension directory (`php-config --extension-dir`), or reference it by its full
path in the next step.

## 2. Turn it on

Add a `conf.d` file that **both php-fpm and the CLI** read. They often use different directories, for example
`/etc/php/8.5/fpm/conf.d` and `/etc/php/8.5/cli/conf.d`; `php --ini` shows which files the CLI reads.

```ini
extension=nocopy               ; PIE may have added this already
nocopy.shm_name=/nocopy        ; required: the name of the shared cache
nocopy.shm_size=128M           ; memory for the cache
```

Without `nocopy.shm_name` there is no cache at all: PHP warns at startup, every read misses and every write returns
`false`. All settings: [Configuration](../reference/configuration.md).

## 3. Restart and check

Restart php-fpm, then check from the CLI:

```sh
php -r 'var_dump(nocopy_shared_with());'     # string(4) "host"
```

and from a web request: `var_dump(nocopy_shared_with());` should also print `host`. If one side prints `none`,
`nocopy_stats()['off']` says why. To check that web and CLI really share one cache (same user, same build), follow
[Share one cache between php-fpm and the CLI](share-between-fpm-and-cli.md).

## In Docker

Containers get a 64 MB `/dev/shm` by default, and nocopy reserves its whole `nocopy.shm_size` there up front. Give the
container more:

```sh
docker run --shm-size=256m …
```

```yaml
# docker-compose.yml
services:
  php:
    shm_size: 256m
```

php-fpm and CLI commands (`docker exec`) in the **same** container share the cache. Separate containers do not, unless
they share `/dev/shm` (`--ipc=container:…`, or `ipc: service:php` in compose) and run the same `nocopy.so` as the same uid.
For Kubernetes: [Run nocopy on Kubernetes](kubernetes.md).

## Upgrade

1. Install the new version (with PIE: `pie install chrfritsch/nocopy:<version>`).
2. Restart php-fpm (or FrankenPHP) and any long-running CLI workers.

The cache starts empty after an upgrade: each build gets its own shared-memory object, because its name includes a hash
of `nocopy.so`. Old objects are removed once no process uses them. A process that finds an old one still in use warns
that those processes do not share its cache; restart them.

## Uninstall

Remove the `conf.d` file, restart php-fpm, and remove what is left in `/dev/shm`: `rm /dev/shm/nocopy.*` (the name you
configured, followed by `.*`).
