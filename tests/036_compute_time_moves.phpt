--TEST--
nocopy: nocopy_remember's compute time survives a merge moving the entry (XFetch still refreshes early)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t036
apc.shm_size=2M
--FILE--
<?php
nocopy_remember('k', function () { usleep(20000); return range(1, 50); }, 1000, 0);
$moved = nocopy_stats()['evacuated_bytes'];
for ($i = 0; $i < 3000; $i++) { // churn; 'k' is read often, so merges move it instead of evicting it
    nocopy_put("t$i", str_repeat('z', 2000), 1000); // same TTL bucket as k
    if ($i % 20 == 0) nocopy_get('k');
}
nocopy_get('k', $hit);
var_dump($hit, nocopy_stats()['evacuated_bytes'] > $moved);
$ran = false;
nocopy_remember('k', function () use (&$ran) { $ran = true; return 1; }, 1000, 1e9); // ~20 ms * 1e9: refreshes now if the time was kept
var_dump($ran);
$ran = false;
nocopy_remember('k', function () use (&$ran) { $ran = true; return 2; }, 1000, 1e9); // the fresh value's compute time is ~0 ms
var_dump($ran);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t036*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(false)
