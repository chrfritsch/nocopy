<?php
// Baseline for shared caches: drupal/redis (phpredis) for all bins and tags, core's APCu chainedfast in front of bootstrap/config/discovery.
// igbinary like the nocopy configs.
require_once __DIR__ . '/IgbinarySerialize.php';
$class_loader->addPsr4('Drupal\\redis\\', $app_root . '/modules/contrib/redis/src');
$settings['redis.connection']['interface'] = 'PhpRedis';
$settings['redis.connection']['host'] = '127.0.0.1';
$settings['cache']['default'] = 'cache.backend.redis';
$settings['container_yamls'][] = $app_root . '/modules/contrib/redis/example.services.yml';
$settings['container_yamls'][] = __DIR__ . '/redis.igbinary.services.yml';
$settings['bootstrap_container_definition'] = [
  'parameters' => [],
  'services' => [
    'redis.factory' => ['class' => 'Drupal\redis\ClientFactory'],
    'cache.backend.redis' => ['class' => 'Drupal\redis\Cache\CacheBackendFactory', 'arguments' => ['@redis.factory', '@cache_tags_provider.container', '@serialization.igbinary']],
    'cache.container' => ['class' => '\Drupal\redis\Cache\PhpRedis', 'factory' => ['@cache.backend.redis', 'get'], 'arguments' => ['container']],
    'cache_tags_provider.container' => ['class' => 'Drupal\redis\Cache\RedisCacheTagsChecksum', 'arguments' => ['@redis.factory']],
    'serialization.igbinary' => ['class' => 'NocopyBench\IgbinarySerialize'],
  ],
];
