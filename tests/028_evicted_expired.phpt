--TEST--
nocopy: nocopy_stats() counts entries evicted by merge and entries dropped by their TTL
--EXTENSIONS--
nocopy
--INI--
apc.shm_size=4M
--FILE--
<?php
for ($i = 0; $i < 100; $i++) nocopy_put("t$i", 1, 1);
sleep(2);
$v = str_repeat('x', 500);
for ($i = 0; $i < 30000; $i++) nocopy_put("k$i", $v); // ~18M through 4M
$s = nocopy_stats();
var_dump($s['expired'], $s['evicted'] > 20000, $s['evicted'] + $s['expired'] + $s['entries']); // every store accounted for
?>
--EXPECT--
int(100)
bool(true)
int(30100)
