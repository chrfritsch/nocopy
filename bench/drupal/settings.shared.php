<?php
// All bins and cache tag checksums in nocopy, shared by every process (nocopy.shm_name or nocopy.valkey): no journal.
require __DIR__ . '/settings.all.php';
$settings['nocopy_shared'] = TRUE;
