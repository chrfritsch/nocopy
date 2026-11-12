# nocopy for Drupal

A Drupal cache backend on the [nocopy](https://github.com/chrfritsch/nocopy) PHP extension.

- **All cache bins, cache tag checksums, locks and flood control** move out of the database into shared memory. Cache
  reads, tag checks, locks and flood checks no longer run a query.
- **One cache for the whole site:** php-fpm, Drush and cron share it, and it survives php-fpm restarts. On one server
  through `nocopy.shm_name`, on several through `nocopy.valkey`.
- **No copying:** cached arrays (config, the container definition, most bootstrap data) are read without a copy.

## Install

[Install the extension](../docs/how-to/install.md) for php-fpm **and** the CLI, with the same settings in both. They
usually read different php.ini files, so check both:

```ini
nocopy.shm_name=/nocopy        ; required: Drush and cron attach to php-fpm's memory
nocopy.serializer=igbinary     ; recommended, if you have igbinary: render arrays contain objects, stored serialized
;nocopy.valkey=host:6379       ; several servers: Valkey is the shared cache, each server keeps local copies
```

Then require the module and the polyfill (not on Packagist yet, so from a checkout of the nocopy repository) and enable it:

```sh
composer config repositories.nocopy path /path/to/nocopy/drupal
composer config repositories.nocopy-apcu path /path/to/nocopy/apcu
composer require chrfritsch/nocopy-drupal:@dev chrfritsch/nocopy-apcu:@dev
drush en nocopy
```

Why the `apcu_*` polyfill: without APCu, Drupal's class loader and FileCache look for `apcu_fetch()` and fall back to the
file system. Measured on 2,000 nodes with 8 workers: 807 `faccessat` calls per logged-in request without the polyfill, 65
with it; logged-in pages +21-31% faster, anonymous ones +11-39% (median of 3 interleaved runs).

Run Drush and cron as the user php-fpm's (or Apache's) workers run as: only that user can open the cache
([why](../docs/how-to/share-between-fpm-and-cli.md#2-run-everything-as-one-user)).

## settings.php

Add this to `settings.php` (adjust the path if the module is not in `modules/contrib/nocopy`):

```php
// nocopy for all bins and cache tag checksums.
$class_loader->addPsr4('Drupal\\nocopy\\', $app_root . '/modules/contrib/nocopy/src');
$settings['container_yamls'][] = $app_root . '/modules/contrib/nocopy/example.services.yml';
$settings['cache']['default'] = 'cache.backend.nocopy';
// These bins default to chainedfast, which outranks 'default'.
foreach (['bootstrap', 'config', 'discovery'] as $bin) {
  $settings['cache']['bins'][$bin] = 'cache.backend.nocopy';
}
// The container bin is read before the container exists. Settings is not initialized yet, so a literal prefix.
$settings['bootstrap_container_definition'] = [
  'parameters' => [],
  'services' => [
    'datetime.time' => ['class' => 'Drupal\Component\Datetime\Time'],
    'cache_tags_provider.container' => [
      'class' => 'Drupal\nocopy\Cache\NocopyCacheTagsChecksum',
      'arguments' => [$app_root, $site_path],
    ],
    'cache.container' => [
      'class' => 'Drupal\nocopy\Cache\NocopyBackend',
      'arguments' => ['container', 'nocopy_container.' . hash('xxh128', $app_root . '/' . $site_path), '@cache_tags_provider.container', '@datetime.time'],
    ],
  ],
];
```

## Check it

The status report (`admin/reports/status`) shows whether this PHP process shares the cache, its memory use and evictions.

Each process also checks, on its first cache access, that it shares the cache with the rest of the site
(`nocopy_shared_with()` is `host` or `valkey`). If not, it throws an exception that names the cause, rather than serve data
other processes have already changed.

## Several web servers

With `nocopy.valkey`, all bins and tag checksums are shared through Valkey, and each server keeps zero-copy local copies
([Valkey guide](../docs/how-to/valkey.md)).

Give Valkey a `maxmemory` and `maxmemory-policy volatile-lfu`. In this mode, permanent cache items get a one-year TTL, so
Valkey may evict them; cache tag tokens, bin markers and the small bins (container, bootstrap, config, discovery) get no
TTL and stay. Invalidated items are not deleted (as in core), so without a memory limit Valkey only grows.

## Key prefix

The prefix is derived like core's APCu prefix: from the app root, `deployment_identifier` and the Drupal version. So it
changes with each deploy and core update. With `nocopy.valkey`, the old tag tokens and small bins then stay in Valkey,
since they have no TTL. If every server uses the same docroot, set a fixed prefix:

```php
$settings['nocopy_prefix'] = 'mysite';
```

## Fast layer only

To keep the database as the cache of record and only replace APCu in front of it, use
[`bench/drupal/settings.fast.php`](../bench/drupal/settings.fast.php) and
[`fast.services.yml`](../bench/drupal/fast.services.yml) instead of the settings above. They use
`cache.backend.nocopy_fast`, which works on several web servers without Valkey (chainedfast already checks the database on
every request). Like core's APCu fast layer, it falls back to the database alone where the extension is not loaded or
`nocopy.shm_name` is not set.

## Trade-offs

- **Without `nocopy.valkey`: single server only.** Each server has its own shared memory, so caches and tag invalidations are not shared.
- **Long-running CLI processes** (queue workers, migrations) in copy mode see changes made elsewhere on their next cache read, like web requests.
- **Deletes inside a database transaction wait for the commit** (none on rollback), so a concurrent request cannot refill the cache from uncommitted data.
- **Locks** (`lock`, `lock.persistent`): their keys have no TTL and no deploy in the prefix, so Valkey's `volatile-*` eviction never
  drops a held lock and old and new code share locks during a rolling deploy. Without Valkey a full arena can evict one, like any cache.
  Expiry is wall time: run NTP.
- **Flood control** counts in a fixed window from the first event (as Laravel's rate limiter): up to twice the threshold across its edge.
  It does not implement `PrefixFloodInterface`, so a password change does not clear `user.failed_login_user` (as with `drupal/redis`).
- **Memory:** `nocopy.shm_size` must hold the cache, render cache included. When it is full, nocopy evicts entries (cache misses, not errors).
  Check `nocopy_stats()['used']` after warm-up: the 2,000-node benchmark site used 11 MB, so the default `32M` fits; raise it once
  `used` passes about half.
- **`nocopy.local_size`** (copy mode: Drush, queue workers, worker runtimes) can stay at `8M`: a request's hot entries on the benchmark site
  were about 3 MB, and a simulated worker (one process, a new kernel per request) was as fast with `0` as with `64M` (110-170 ms
  either way: cache reads are noise next to the rest of a Drupal request).
