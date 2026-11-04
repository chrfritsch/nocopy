# nocopy for Drupal

Cache backend and cache tag checksums on the [nocopy](https://github.com/chrfritsch/nocopy) PHP extension, for **single-server** sites.
All cache bins and tag checksums live in shared memory, so cache reads and tag checks no longer touch the database. Cached arrays
(config, the container definition, most bootstrap data) are read without a copy.

## Install

The nocopy extension must be loaded in php-fpm (and in the CLI if Drush should see the same backend classes). Then:

```sh
composer config repositories.nocopy path /path/to/nocopy/drupal
composer require chrfritsch/nocopy-drupal:@dev
drush en nocopy
```

## settings.php

```php
// nocopy for all bins and cache tag checksums (single server).
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

**Fast layer only:** to keep the database as the cache of record and only replace APCu in front of it, use
[`bench/drupal/settings.fast.php`](../bench/drupal/settings.fast.php) and [`fast.services.yml`](../bench/drupal/fast.services.yml) instead.
It uses `cache.backend.nocopy_fast`, which keeps no journal (chainedfast already checks the database on every request) and falls back to
the database alone where the extension is not loaded, like core's APCu fast layer. Works on several web servers.

## Trade-offs

- **Single server only.** Each server has its own shared memory, so caches and tag invalidations are not shared.
- **Drush, cron and other CLI processes have their own memory.** They log every cache write, cache clear and tag invalidation
  to the `nocopy_journal` table. Each web request reads the journal's last id (one query, also on page cache hits) and replays what is new,
  so `drush cr`, `drush cim`, `drush updb` and content saved by `drush cron` reach php-fpm on the next request. Cron drops entries
  older than a day, and a web worker that missed dropped entries starts with an empty cache. Long-running CLI processes (queue
  workers, migrations) do not see web changes made while they run.
- **Do not set `nocopy.ttl`** (or `apc.ttl`): it would expire cache tag tokens, and a lost token for a common tag such as `rendered`
  invalidates most of the site at once.
- **The cache is empty after a php-fpm restart or reload,** so the first requests rebuild it. In the benchmark, the first logged-in
  request after a restart took 476–544 ms (stock: 173–248 ms, where the database caches survive), and requests were back to normal from the third one.
- **Memory:** `nocopy.shm_size` must hold the cache, render cache included. When it is full, nocopy evicts entries (cache misses, not errors).
  Check `nocopy_stats()['used']` after warm-up: the 2,000-node benchmark site used 11 MB.
