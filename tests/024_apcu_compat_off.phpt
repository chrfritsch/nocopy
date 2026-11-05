--TEST--
nocopy: nocopy.apcu_compat=0 drops apcu_*, APCUIterator and apc.*; nocopy.* sizes the arena
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t024
nocopy.apcu_compat=0
nocopy.shm_size=4M
--FILE--
<?php
var_dump(function_exists('apcu_fetch'), class_exists('APCUIterator', false), defined('APC_ITER_ALL'), ini_get('apc.shm_size'));
var_dump(nocopy_stats()['size'], nocopy_put('k', [1]), nocopy_get('k'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t024*')); ?>
--EXPECT--
bool(false)
bool(false)
bool(false)
bool(false)
int(4194304)
bool(true)
array(1) {
  [0]=>
  int(1)
}
