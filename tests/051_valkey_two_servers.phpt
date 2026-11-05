--TEST--
nocopy.valkey: two servers (separate nocopy memories) share values, and each one's writes and deletes reach the other's local copies
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t051
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t051:
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
require __DIR__ . '/valkey.inc';
apcu_clear_cache();
nocopy_put('k', ['v' => 1]);
var_dump(nocopy_get('k'));
echo other_server('t051:', 'echo json_encode(nocopy_get("k")); nocopy_put("k", ["v" => 2]); nocopy_put("only_b", "b");'), "\n";
settle();
var_dump(nocopy_get('k'), nocopy_get('only_b'));
echo other_server('t051:', 'var_dump(nocopy_delete("k"));');
settle();
var_dump(nocopy_get('k'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t051*')); ?>
--EXPECT--
array(1) {
  ["v"]=>
  int(1)
}
{"v":1}
array(1) {
  ["v"]=>
  int(2)
}
string(1) "b"
bool(true)
NULL
