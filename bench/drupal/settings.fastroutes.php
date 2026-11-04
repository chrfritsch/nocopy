<?php
// nocopy as the fast layer (chainedfast, database behind it), with routes added to the chained bins. Safe on several web servers.
$settings['container_yamls'][] = __DIR__ . '/fast.services.yml';
$settings['cache']['bins']['routes'] = 'cache.backend.chainedfast';
