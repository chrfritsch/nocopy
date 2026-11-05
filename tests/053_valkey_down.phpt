--TEST--
nocopy.valkey: no server means misses and failed writes, with one warning per second
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t053
nocopy.valkey=127.0.0.1:1
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
var_dump(nocopy_put('a', 1), nocopy_get('a', $hit), $hit, nocopy_delete('a'), apcu_inc('n'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t053*')); ?>
--EXPECTF--
Warning: nocopy_put(): nocopy: cannot connect to Valkey (127.0.0.1): Connection refused in %s on line %d
bool(false)
NULL
bool(false)
bool(false)
bool(false)
