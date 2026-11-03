--TEST--
stash: a cache full of entries without TTL evicts its oldest segments instead of refusing writes
--EXTENSIONS--
stash
--INI--
apc.shm_size=8M
--FILE--
<?php
$v = str_repeat("x", 500);
$fail = 0;
for ($i = 0; $i < 200000; $i++) $fail += !stash_put("k$i", $v); // ~100M through 8M
$recent = 0;
for ($i = 199000; $i < 200000; $i++) $recent += stash_get("k$i") === $v;
$s = stash_stats();
var_dump($fail, $recent, stash_get("k0") === null, $s['entries'] < 200000, $s['used'] <= $s['size']);
?>
--EXPECT--
int(0)
int(1000)
bool(true)
bool(true)
bool(true)
