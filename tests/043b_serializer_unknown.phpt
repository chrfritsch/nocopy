--TEST--
nocopy.serializer with an unknown value: startup warning, php serializer
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t043
nocopy.serializer=msgpack
--FILE--
<?php
var_dump(ini_get('nocopy.serializer'));
nocopy_put('o', new ArrayObject([1, 2]), 0, true);
var_dump(nocopy_get('o')->count());
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t043*')); ?>
--EXPECTF--
%AWarning: %snocopy: unknown nocopy.serializer "msgpack", using php%A
string(3) "php"
int(2)
