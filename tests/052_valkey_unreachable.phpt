--TEST--
nocopy.valkey: unreachable means misses and failed writes, one warning per second; a lost connection clears the local copies on reconnect
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
apc.enable_cli=1
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t052:
--FILE--
<?php
require __DIR__ . '/valkey.inc';
nocopy_put('k', 'mine');
nocopy_get('k');
vk('CLIENT', 'KILL', 'SKIPME', 'yes'); // drops nocopy's connection
vk('SET', 't052:k', '2');              // an invalidation nocopy never sees
var_dump(nocopy_get('other'));         // the round trip finds the connection gone
sleep(1);
var_dump(nocopy_get('k'));             // reconnected: local copies cleared, Valkey's value
?>
--EXPECT--
NULL
int(2)
