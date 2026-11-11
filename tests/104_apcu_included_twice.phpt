--TEST--
apcu_*: the polyfill included twice (php -S runs auto_prepend_file for the router and again for the routed script) declares nothing twice
--EXTENSIONS--
nocopy
--INI--
apc.enable_cli=1
nocopy.shm_name=/nocopy-t104
--FILE--
<?php
include __DIR__ . '/../apcu/apcu.php';
include __DIR__ . '/../apcu/apcu.php';
var_dump(apcu_store('a', 1), apcu_fetch('a'), APC_ITER_KEY, class_exists('APCUIterator'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t104*')); ?>
--EXPECT--
bool(true)
int(1)
int(2)
bool(true)
