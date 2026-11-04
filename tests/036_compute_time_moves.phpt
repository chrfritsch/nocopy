--TEST--
stash: stash_remember's compute time survives a merge moving the entry (XFetch still refreshes early)
--EXTENSIONS--
stash
--INI--
apc.shm_size=2M
--FILE--
<?php
stash_remember('k', function () { usleep(20000); return range(1, 50); }, 1000, 0);
$moved = stash_stats()['evacuated_bytes'];
for ($i = 0; $i < 3000; $i++) { // churn; 'k' is read often, so merges move it instead of evicting it
    stash_put("t$i", str_repeat('z', 2000), 1000); // same TTL bucket as k
    if ($i % 20 == 0) stash_get('k');
}
stash_get('k', $hit);
var_dump($hit, stash_stats()['evacuated_bytes'] > $moved);
$ran = false;
stash_remember('k', function () use (&$ran) { $ran = true; return 1; }, 1000, 1e9); // ~20 ms * 1e9: refreshes now if the time was kept
var_dump($ran);
$ran = false;
stash_remember('k', function () use (&$ran) { $ran = true; return 2; }, 1000, 1e9); // the fresh value's compute time is ~0 ms
var_dump($ran);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(false)
