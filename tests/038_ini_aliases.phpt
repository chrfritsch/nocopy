--TEST--
stash: stash.* sizing INI is canonical; apc.* are aliases with the APCu layer (used when stash.* is unset; ini_get shows the value in effect)
--EXTENSIONS--
stash
--INI--
apc.enable_cli=1
stash.shm_size=8M
apc.shm_size=4M
apc.ttl=7
stash.entries_hint=1000
--FILE--
<?php
var_dump(stash_stats()['size'], ini_get('stash.shm_size'), ini_get('apc.shm_size'));
var_dump(ini_get('stash.ttl'), ini_get('apc.ttl'), ini_get('apc.entries_hint'));
apcu_store('k', 1); // apc.ttl=7 through the alias
var_dump(apcu_key_info('k')['ttl']);
?>
--EXPECTF--
%AWarning: %sstash: apc.shm_size is ignored, stash.shm_size is set%A
int(8388608)
string(2) "8M"
string(2) "8M"
string(1) "7"
string(1) "7"
string(4) "1000"
int(7)
