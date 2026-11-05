--TEST--
nocopy: nocopy_put(..., serialize: true) stores objects serialized
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t025
--FILE--
<?php
$o = new ArrayObject(['a' => 1]);
var_dump(nocopy_put('o', ['obj' => $o], 0, true));
$v = nocopy_get('o', $hit);
var_dump($hit, $v['obj'] == $o, $v['obj'] !== $o);
try { nocopy_put('p', $o); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t025*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
nocopy_put(): Argument #2 ($value) must not contain objects or resources
