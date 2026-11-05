--TEST--
nocopy: keys that come back after an eviction start hot (ghost), so keys reused at ~1.25x the cache size get hits
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t033
apc.shm_size=4M
--FILE--
<?php
$v = str_repeat('x', 500);
$hits = $n = $u = 0;
for ($cycle = 0; $cycle < 12; $cycle++) {
    for ($j = 0; $j < 1400; $j++) {
        for ($s = 0; $s < 5; $s++) nocopy_put("u" . $u++, $v); // one-hit wonders
        nocopy_get("r$j", $hit);
        if ($cycle >= 5) { $n++; $hits += $hit; }
        if (!$hit) nocopy_put("r$j", $v);
    }
}
var_dump($hits / $n > 0.3, nocopy_stats()['ghost_hits'] > 1000); // without the ghost: 0 hits
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t033*')); ?>
--EXPECT--
bool(true)
bool(true)
