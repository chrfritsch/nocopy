--TEST--
nocopy.valkey: with the listener unable to connect (CLIENT TRACKING denied), only the first call waits nocopy.valkey_timeout, the next ones miss at once
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t084
nocopy.valkey=tcp://u084:pw@127.0.0.1:6379
nocopy.valkey_prefix=t084:
nocopy.valkey_timeout=0.3
--CONFLICTS--
valkey
--FILE--
<?php
require __DIR__ . '/valkey.inc';
vk('ACL', 'SETUSER', 'u084', 'reset', 'on', '>pw', '~*', '&*', '+@all', '-client|tracking');
$t = microtime(true);
for ($i = 0; $i < 20; $i++) {
    nocopy_get('k');
}
var_dump(microtime(true) - $t < 1.0);
?>
--CLEAN--
<?php
require __DIR__ . '/valkey.inc';
vk('ACL', 'DELUSER', 'u084');
array_map('unlink', glob('/dev/shm/nocopy-t084*'));
?>
--EXPECTF--
Warning: nocopy_get(): nocopy: the Valkey listener is not connected: no invalidations, local copies are not served in %s on line %d
bool(true)
