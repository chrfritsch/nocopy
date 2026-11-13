--TEST--
nocopy.resp: nocopy_delete_multiple sends one UNLINK and drops the local copies
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/resp_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t066
nocopy.resp=127.0.0.1:6379
nocopy.resp_prefix=t066:
--CONFLICTS--
resp
--FILE--
<?php
require __DIR__ . '/resp.inc';
nocopy_clear();
nocopy_put_multiple(['a' => 1, 'b' => [2], 'c' => 3]);
$got = nocopy_get_multiple(['a', 'b', 'c']);
vk('CONFIG', 'RESETSTAT');
var_dump(nocopy_delete_multiple(['a', 'missing', 'b']));
var_dump(preg_match('/cmdstat_unlink:calls=1,/', vk('INFO', 'commandstats')));
var_dump(vk('EXISTS', 't066:a'), vk('EXISTS', 't066:c'), nocopy_get_multiple(['a', 'b', 'c']));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t066*')); ?>
--EXPECT--
int(2)
int(1)
int(0)
int(1)
array(1) {
  ["c"]=>
  int(3)
}
