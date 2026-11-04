--TEST--
apc.serializer is an alias of nocopy.serializer; ini_get shows the value in effect
--EXTENSIONS--
nocopy
igbinary
--INI--
apc.enable_cli=1
apc.serializer=igbinary
--FILE--
<?php
var_dump(ini_get('nocopy.serializer'), ini_get('apc.serializer'));
apcu_store('o', new ArrayObject([1, 2]));
var_dump(apcu_fetch('o')->count());
?>
--EXPECT--
string(8) "igbinary"
string(8) "igbinary"
int(2)
