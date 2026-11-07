--TEST--
nocopy.valkey: get_multiple serves local hits and fetches every miss in one MGET; put_multiple pipelines the SETs with the TTL
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t055
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t055:
--CONFLICTS--
valkey
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
require __DIR__ . '/valkey.inc';
apcu_clear_cache();
var_dump(nocopy_put_multiple(['a' => [1, 2], 'n' => 5, 'o' => new ArrayObject([3])], 60));
var_dump(ord(vk('GET', 't055:a')[0]), vk('GET', 't055:n'), vk('PTTL', 't055:a') > 50000);

// another server's writes: only in Valkey, not local here
other_server('t055:', 'nocopy_put("r1", ["x"]); nocopy_put("r2", 9);');
settle();
vk('CONFIG', 'RESETSTAT');
$got = nocopy_get_multiple(['a', 'r1', 'gone', 'r2', 'n', 'o']);
var_dump(array_keys($got), $got['r1'], $got['r2'], $got['o'] instanceof ArrayObject);
var_dump(preg_match('/cmdstat_mget:calls=1,/', vk('INFO', 'commandstats')), preg_match('/cmdstat_get:/', vk('INFO', 'commandstats')));

// filled locally: the next batch makes no round trip
vk('CONFIG', 'RESETSTAT');
nocopy_get_multiple(['r1', 'r2']);
var_dump(preg_match('/cmdstat_mget:/', vk('INFO', 'commandstats')));

// a change elsewhere drops the filled copy
vk('SET', 't055:r2', '10');
settle();
var_dump(nocopy_get_multiple(['r2']));

var_dump(apcu_store(['s1' => 1, 's2' => 2]), vk('GET', 't055:s2'), apcu_fetch(['s1', 's2']));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t055*')); ?>
--EXPECT--
bool(true)
int(0)
string(1) "5"
bool(true)
array(5) {
  [0]=>
  string(1) "a"
  [1]=>
  string(2) "r1"
  [2]=>
  string(2) "r2"
  [3]=>
  string(1) "n"
  [4]=>
  string(1) "o"
}
array(1) {
  [0]=>
  string(1) "x"
}
int(9)
bool(true)
int(1)
int(0)
int(0)
array(1) {
  ["r2"]=>
  int(10)
}
array(0) {
}
string(1) "2"
array(2) {
  ["s1"]=>
  int(1)
  ["s2"]=>
  int(2)
}
