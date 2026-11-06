<?php
// nocopy as the fast layer only: chainedfast bins use it in front of the database.
require '/src/apcu/apcu.php'; // as composer require chrfritsch/nocopy-drupal loads it: Composer's class loader and FileCache then cache in nocopy
$settings['container_yamls'][] = __DIR__ . '/fast.services.yml';
