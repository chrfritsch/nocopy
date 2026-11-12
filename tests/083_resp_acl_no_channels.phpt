--TEST--
nocopy.resp: a user refused SUBSCRIBE (NOPERM) still gets each key's own value; the refusal no longer puts every later reply one behind
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/resp_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t083
nocopy.resp=tcp://u083:pw@127.0.0.1:6379
nocopy.resp_prefix=t083:
--CONFLICTS--
resp
--FILE--
<?php
require __DIR__ . '/resp.inc';
vk('ACL', 'SETUSER', 'u083', 'reset', 'on', '>pw', '~*', '+@all', 'resetchannels');
vk('SET', 't083:a', '1');
vk('SET', 't083:b', '2');
vk('SET', 't083:c', '3');
var_dump(nocopy_get('a'), nocopy_get('b'), nocopy_get('c'), nocopy_put('d', 'x'), nocopy_get('d'));
?>
--CLEAN--
<?php
require __DIR__ . '/resp.inc';
vk('ACL', 'DELUSER', 'u083');
array_map('unlink', glob('/dev/shm/nocopy-t083*'));
?>
--EXPECT--
int(1)
int(2)
int(3)
bool(true)
string(1) "x"
