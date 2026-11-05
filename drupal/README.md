# nocopy for Drupal

Cache backend and cache tag checksums on the [nocopy](https://github.com/chrfritsch/nocopy) PHP extension. Every PHP process of the site
(php-fpm, Drush, cron) shares one cache: `nocopy.shm_name` on a single server, `nocopy.valkey` on several.
All cache bins and tag checksums live in shared memory, so cache reads and tag checks no longer touch the database. Cached arrays
(config, the container definition, most bootstrap data) are read without a copy.

## Install

The nocopy extension must be loaded in php-fpm and in the CLI, both with the same shared cache in php.ini:

```ini
nocopy.shm_name=/nocopy        ; one server: Drush and cron attach to php-fpm's memory
;nocopy.valkey=host:6379       ; several servers: Valkey is the shared cache, each server keeps local copies
```

Then:

```sh
composer config repositories.nocopy path /path/to/nocopy/drupal
composer require chrfritsch/nocopy-drupal:@dev
drush en nocopy
```

## settings.php

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

Recommended in php.ini: `nocopy.serializer=igbinary` (cached render arrays contain objects, which are stored serialized).

Each process checks on its first cache access that it shares the cache with the rest of the site (`nocopy_shared_with()` is `host` or
`valkey`) and throws otherwise, naming the cause, instead of serving what other processes already changed. php-fpm and the CLI usually
read different php.ini files, so set it in both. Run Drush and cron as the user that starts php-fpm (the shared memory object is readable
by its creator only). Writes and invalidations by Drush and cron then reach web requests directly, and with `nocopy.shm_name` the cache
survives php-fpm restarts.

**Several web servers** (`nocopy.valkey`): all bins and tag checksums are shared through Valkey, each server keeps zero-copy local copies.
Give Valkey a `maxmemory` and `maxmemory-policy volatile-lfu`: permanent items get a one-year TTL in that mode and can be evicted, while
cache tag tokens, bin markers and the small bins (container, bootstrap, config, discovery) have none and stay. Invalidated items are not
deleted (as in core), so without a limit Valkey only grows.

**Fast layer only:** to keep the database as the cache of record and only replace APCu in front of it, use
[`bench/drupal/settings.fast.php`](../bench/drupal/settings.fast.php) and [`fast.services.yml`](../bench/drupal/fast.services.yml) instead.
It uses `cache.backend.nocopy_fast`, which needs no shared cache (chainedfast already checks the database on every request) and falls back to
the database alone where the extension is not loaded, like core's APCu fast layer. Works on several web servers.

**Upgrading from a version with the journal:** `drush updb` drops the `nocopy_journal` table; `$settings['nocopy_shared']` is no longer read.

## Trade-offs

- **Without `nocopy.valkey`: single server only.** Each server has its own shared memory, so caches and tag invalidations are not shared.
- **Long-running CLI processes** (queue workers, migrations) in copy mode see changes made elsewhere on their next cache read, like web requests.
- **Deletes inside a database transaction wait for the commit** (none on rollback), so a concurrent request cannot refill the cache from uncommitted data.
- **Do not set `nocopy.ttl`** (or `apc.ttl`): it would expire cache tag tokens, and a lost token for a common tag such as `rendered`
  invalidates most of the site at once.
- **Memory:** `nocopy.shm_size` must hold the cache, render cache included. When it is full, nocopy evicts entries (cache misses, not errors).
  Check `nocopy_stats()['used']` after warm-up: the 2,000-node benchmark site used 11 MB.
