--TEST--
nocopy.serializer=igbinary without igbinary loaded: startup warning, php serializer
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (extension_loaded('igbinary')) die('skip igbinary is loaded'); ?>
--INI--
nocopy.shm_name=/nocopy-t043
nocopy.serializer=igbinary
--FILE--
<?php
var_dump(ini_get('nocopy.serializer'));
nocopy_put('o', new ArrayObject([1, 2]), 0, true);
var_dump(nocopy_get('o')->count());
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t043*')); ?>
--EXPECTF--
%AWarning: %snocopy: igbinary is not loaded, using nocopy.serializer=php%A
string(3) "php"
int(2)
