--TEST--
nocopy.valkey: a URL with a password and no user (redis://:secret@host, rediss://:secret@host) authenticates as the default user
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t114
nocopy.valkey=tcp://:anything@127.0.0.1:6379
nocopy.valkey_prefix=t114:
--CONFLICTS--
valkey
--FILE--
<?php
require __DIR__ . '/valkey.inc';
vk('SET', 't114:a', '1');
var_dump(nocopy_get('a'), nocopy_put('b', 'x'), vk('EXISTS', 't114:b'));
foreach (['redis://127.0.0.1:6379', 'redis://default:anything@127.0.0.1:6379'] as $url) {
    echo php_run('var_dump(nocopy_get("a"));', ['nocopy.shm_name' => '/nocopy-t114-other', 'nocopy.valkey' => $url, 'nocopy.valkey_prefix' => 't114:']);
}
?>
--CLEAN--
<?php
require __DIR__ . '/valkey.inc';
vk('DEL', 't114:a', 't114:b');
array_map('unlink', glob('/dev/shm/nocopy-t114*'));
?>
--EXPECT--
int(1)
bool(true)
int(1)
int(1)
int(1)
