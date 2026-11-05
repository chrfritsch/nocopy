--TEST--
nocopy: nocopy.* sizing INI is canonical; apc.* are aliases with the APCu layer (used when nocopy.* is unset; ini_get shows the value in effect)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t038
apc.enable_cli=1
nocopy.shm_size=8M
apc.shm_size=4M
apc.ttl=7
nocopy.entries_hint=1000
--FILE--
<?php
var_dump(nocopy_stats()['size'], ini_get('nocopy.shm_size'), ini_get('apc.shm_size'));
var_dump(ini_get('nocopy.ttl'), ini_get('apc.ttl'), ini_get('apc.entries_hint'));
apcu_store('k', 1); // apc.ttl=7 through the alias
var_dump(apcu_key_info('k')['ttl']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t038*')); ?>
--EXPECTF--
%AWarning: %snocopy: apc.shm_size is ignored, nocopy.shm_size is set%A
int(8388608)
string(2) "8M"
string(2) "8M"
string(1) "7"
string(1) "7"
string(4) "1000"
int(7)
