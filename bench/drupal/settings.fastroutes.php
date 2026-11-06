<?php
// nocopy as the fast layer (chainedfast, database behind it), with routes added to the chained bins. Safe on several web servers.
require '/src/apcu/apcu.php'; // as composer require chrfritsch/nocopy-drupal loads it: Composer's class loader and FileCache then cache in nocopy
$settings['container_yamls'][] = __DIR__ . '/fast.services.yml';
$settings['cache']['bins']['routes'] = 'cache.backend.chainedfast';
