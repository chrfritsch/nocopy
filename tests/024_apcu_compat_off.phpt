--TEST--
stash: stash.apcu_compat=0 drops apcu_*, APCUIterator and apc.*; stash.* sizes the arena
--EXTENSIONS--
stash
--INI--
stash.apcu_compat=0
stash.shm_size=4M
--FILE--
<?php
var_dump(function_exists('apcu_fetch'), class_exists('APCUIterator', false), defined('APC_ITER_ALL'), ini_get('apc.shm_size'));
var_dump(stash_stats()['size'], stash_put('k', [1]), stash_get('k'));
?>
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
