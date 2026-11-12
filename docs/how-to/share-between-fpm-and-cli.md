# Share one cache between php-fpm and the CLI

Goal: web requests, CLI commands (Drush, artisan, `bin/console`), cron jobs and queue workers on one server all read and
write the same cache, and the cache survives php-fpm restarts.

Three things must match: the name, the user and the build.

## 1. Set the same name everywhere

In a `conf.d` file read by php-fpm **and** by the CLI:

```ini
extension=nocopy.so
nocopy.shm_name=/nocopy
nocopy.shm_size=128M
```

Two sites on one server that must not see each other's keys need different names, such as `/nocopy-shop` and
`/nocopy-blog` (set per php-fpm pool, see below).

## 2. Run everything as one user

The first process that needs the cache creates it, readable and writable only by its own user (mode `0600`). Every other
process must run as that same user. With php-fpm that is the pool's user, for example `www-data`: the workers open the
cache, not the master (which often runs as root).

So run CLI commands as the pool's user:

```sh
sudo -u www-data php bin/console cache:warmup
```

A process running as another user, root included, cannot open the cache: it logs a warning and has no cache.
Do not try to loosen the permissions: anyone who can write to the cache controls what php-fpm reads
([why](../explanation/sharing.md#security-one-user-is-one-trust-domain)).

Pools running as different users need different names, set per pool:

```ini
; php-fpm.d/shop.conf
php_admin_value[nocopy.shm_name] = /nocopy-shop
```

## 3. Load the same build

php-fpm and the CLI share the cache only if they load the same `nocopy.so` build (the shared-memory object's name
includes its build id). Point both at the same file, and restart php-fpm after replacing it: until then, it keeps using the
old build's cache.

## 4. Verify

```sh
php -r 'var_dump(nocopy_shared_with()); nocopy_put("probe", getmypid());'
```

Then read `probe` in a web request (`var_dump(nocopy_get('probe'));`): it shows the CLI's pid. On both sides,
`nocopy_shared_with()` must return `host`, and `nocopy_stats()['shm_name']` the same name.

In deploy scripts, make a cache clear that reached nothing fail the deploy. Run it as the pool's user with the pool's
settings:

```sh
sudo -u www-data php -r 'exit(nocopy_clear() ? 0 : 1);'   # or: cli/nocopy clear (exits non-zero)
```

## Troubleshooting

The warning in the error log tells you what is wrong:

| Symptom | Cause |
|---|---|
| `nocopy.shm_name is not set` | this process reads another php.ini without the setting (`php --ini` shows which) |
| `no permission to open it (another user created it)` | the process runs as a different user than the creator |
| `it is owned by another user or open to others` | the object belongs to another user (also when you run as root: use the pool's user) or its mode was widened: check `ls -l /dev/shm`, remove it if stale |
| `… the nocopy.shm_name cache of another PHP or nocopy build, is in use: its processes do not share this cache` | php-fpm still runs an older `nocopy.so`: restart it |
| `not enough room in /dev/shm (Docker: --shm-size)` | `/dev/shm` smaller than `nocopy.shm_size` |
| `not a name like /nocopy …` | the name needs one leading slash, at most 40 characters |
| `no file descriptor or no free process slot for its liveness lock` | the process is at its fd limit (`ulimit -n`), or 1024 processes have the cache open |
| `no worker slot: …` | 1024 processes and threads have the cache open, or a forked process had no file descriptor left |
| `nocopy.shm_size or nocopy.entries_hint ignored: …` | a process still using the cache created it with other settings: stop every process using it (php-fpm, CLI workers, cron) to resize |
| CLI and web disagree although names match | different containers: they share only through a common `/dev/shm`, with the same `nocopy.so` and uid |

To share between servers, see [Valkey](valkey.md). Background: [Sharing: by name and through Valkey](../explanation/sharing.md).
