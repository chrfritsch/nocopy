--TEST--
nocopy.valkey: a get_multiple whose later key holds a broken payload keeps the values read before it intact
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t112
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t112:
--CONFLICTS--
valkey
--FILE--
<?php
require __DIR__ . '/valkey.inc';
nocopy_clear();
nocopy_put('a', ['x' => str_repeat('y', 40)]);
nocopy_put('b', 'v');
vk('SET', 't112:a', vk('GET', 't112:a')); // another client's write: the local copies go
vk('SETRANGE', 't112:b', '14', '!');      // b's serialized payload broken
settle();
$r = nocopy_get_multiple(['a', 'b']);
var_dump(array_keys($r), $r['a']['x'] === str_repeat('y', 40));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t112*')); ?>
--EXPECT--
array(1) {
  [0]=>
  string(1) "a"
}
bool(true)
