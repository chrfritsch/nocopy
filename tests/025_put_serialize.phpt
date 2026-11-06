--TEST--
nocopy: objects are stored serialized, resources refused with a warning
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t025
--FILE--
<?php
$o = new ArrayObject(['a' => 1]);
var_dump(nocopy_put('o', ['obj' => $o], 0));
$v = nocopy_get('o', $hit);
var_dump($hit, $v['obj'] == $o, $v['obj'] !== $o);
var_dump(nocopy_put('p', $o), nocopy_get('p') == $o, nocopy_put('f', [STDIN]));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t025*')); ?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
bool(true)

Warning: nocopy_put(): nocopy: cannot store resources in %s on line %d
bool(true)
bool(true)
bool(false)
