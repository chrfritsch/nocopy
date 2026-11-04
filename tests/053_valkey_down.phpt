--TEST--
nocopy.valkey: no server means misses and failed writes, with one warning per second
--EXTENSIONS--
nocopy
--INI--
nocopy.valkey=127.0.0.1:1
--FILE--
<?php
var_dump(nocopy_put('a', 1), nocopy_get('a', $hit), $hit, nocopy_delete('a'), apcu_inc('n'));
?>
--EXPECTF--
Warning: nocopy_put(): nocopy: cannot connect to Valkey (127.0.0.1): Connection refused in %s on line %d
bool(false)
NULL
bool(false)
bool(false)
bool(false)
