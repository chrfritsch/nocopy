--TEST--
nocopy: a cache full of entries without TTL evicts its oldest segments instead of refusing writes
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t019
apc.shm_size=8M
--FILE--
<?php
$v = str_repeat("x", 500);
$fail = 0;
for ($i = 0; $i < 200000; $i++) $fail += !nocopy_put("k$i", $v); // ~100M through 8M
$recent = 0;
for ($i = 199000; $i < 200000; $i++) $recent += nocopy_get("k$i") === $v;
$s = nocopy_stats();
var_dump($fail, $recent, nocopy_get("k0") === null, $s['entries'] < 200000, $s['used'] <= $s['size']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t019*')); ?>
--EXPECT--
int(0)
int(1000)
bool(true)
bool(true)
bool(true)
