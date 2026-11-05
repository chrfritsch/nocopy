--TEST--
apc.serializer=default (APCu's name for php) maps to nocopy.serializer=php
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t044
apc.enable_cli=1
apc.serializer=default
--FILE--
<?php
var_dump(ini_get('nocopy.serializer'), ini_get('apc.serializer'));
apcu_store('o', new ArrayObject([1, 2]));
var_dump(apcu_fetch('o')->count());
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t044*')); ?>
--EXPECT--
string(3) "php"
string(3) "php"
int(2)
