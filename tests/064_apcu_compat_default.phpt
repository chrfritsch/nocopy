--TEST--
nocopy: the apcu_* layer is opt-in (nocopy.apcu_compat=1); by default only nocopy_* and nocopy.* exist
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t064
--FILE--
<?php
$out = shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO')) . ' -d nocopy.shm_name=/nocopy-t064 -d apc.shm_size=1M -r '
    . escapeshellarg('var_dump(function_exists("apcu_fetch"), class_exists("APCUIterator", false), ini_get("apc.shm_size"), ini_get("nocopy.apcu_compat"), nocopy_put("k", 1), nocopy_get("k"), nocopy_stats()["size"] > 1 << 20);') . ' 2>&1');
echo $out;
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t064*')); ?>
--EXPECT--
bool(false)
bool(false)
bool(false)
string(1) "0"
bool(true)
int(1)
bool(true)
