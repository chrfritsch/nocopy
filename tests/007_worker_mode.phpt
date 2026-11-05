--TEST--
nocopy: worker mode (request_scope=0) fetches private copies and holds no pins
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t007
nocopy.request_scope=0
--FILE--
<?php
$orig = ['name' => 'orig', 'list' => range(1, 300), 'nested' => ['a' => 'b']];
nocopy_put('k', $orig);
$held = nocopy_get('k');
ob_start(); debug_zval_dump($held); $d = ob_get_clean();
var_dump(str_contains($d, 'interned'));
for ($i = 0; $i < 300; $i++) nocopy_put('churn', range(1, 1000));
for ($i = 0; $i < 30; $i++) nocopy_put('k', array_fill(0, 200, "overwrite-$i")); // recycle freed blocks
$s = nocopy_stats();
var_dump($s['used'] < 300000, $s['pinned_segments'], $held === $orig);
var_dump(function_exists('nocopy_scope'), function_exists('nocopy_checkpoint'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t007*')); ?>
--EXPECT--
bool(false)
bool(true)
int(0)
bool(true)
bool(false)
bool(false)
