<?php
// nocopy for all bins and the container, tags in the database: compared with settings.apcu.php it isolates the copy on read.
$class_loader->addPsr4('Drupal\\nocopy\\', $app_root . '/modules/contrib/nocopy/src');
$settings['cache']['default'] = 'cache.backend.nocopy';
foreach (['bootstrap', 'config', 'discovery'] as $bin) {
  $settings['cache']['bins'][$bin] = 'cache.backend.nocopy';
}
$settings['bootstrap_container_definition'] = [
  'parameters' => [],
  'services' => [
    'database' => ['class' => 'Drupal\Core\Database\Connection', 'factory' => 'Drupal\Core\Database\Database::getConnection', 'arguments' => ['default']],
    'request_stack' => ['class' => 'Symfony\Component\HttpFoundation\RequestStack'],
    'datetime.time' => ['class' => 'Drupal\Component\Datetime\Time', 'arguments' => ['@request_stack']],
    'cache_tags_provider.container' => ['class' => 'Drupal\Core\Cache\DatabaseCacheTagsChecksum', 'arguments' => ['@database']],
    'cache.container' => ['class' => 'Drupal\nocopy\Cache\NocopyBackend', 'arguments' => ['container', 'nocopy_container.' . hash('xxh128', $app_root . '/' . $site_path), '@cache_tags_provider.container', '@datetime.time']],
  ],
];
