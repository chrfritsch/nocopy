--TEST--
nocopy: a TTL near PHP_INT_MAX is capped, not wrapped into the past
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t080
--FILE--
<?php
$t = PHP_INT_MAX - time() + 1;
var_dump(nocopy_put('a', 'v', PHP_INT_MAX), nocopy_get('a'));
var_dump(nocopy_put('b', 'v', $t), nocopy_get('b'));
var_dump(nocopy_add('c', 'v', PHP_INT_MAX), nocopy_get('c'));
var_dump(nocopy_inc('d', 5, PHP_INT_MAX), nocopy_get('d'));
var_dump(nocopy_put_multiple(['e' => 1], PHP_INT_MAX), nocopy_get('e'));
var_dump(nocopy_remember('f', function ($k, &$ttl) { $ttl = PHP_INT_MAX; return 'r'; }), nocopy_get('f'));
$e = nocopy_keys('a')['a']['expires'];
var_dump($e > time() + (1 << 31), $e <= time() + (1 << 32) + 1);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t080*')); ?>
--EXPECT--
bool(true)
string(1) "v"
bool(true)
string(1) "v"
bool(true)
string(1) "v"
int(5)
int(5)
bool(true)
int(1)
string(1) "r"
string(1) "r"
bool(true)
bool(true)
