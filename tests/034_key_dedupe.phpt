--TEST--
nocopy: equal array keys are stored once per value and copied out once; relocation keeps them intact
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t034
nocopy.shm_size=2M
nocopy.request_scope=0
--FILE--
<?php
$rows = [];
for ($i = 0; $i < 500; $i++) $rows["r$i"] = ['id' => $i, 'name' => "n$i", "k$i" => $i % 7];
$fresh = unserialize(serialize($rows)); // same keys, every one a separate string

$used = fn() => nocopy_stats()['used'];
$u0 = $used(); nocopy_put('a', $rows); $ua = $used() - $u0;
$u0 = $used(); nocopy_put('b', $fresh); $ub = $used() - $u0;
var_dump($ua === $ub, nocopy_get('a') === $rows, nocopy_get('b') === $rows);

$wide = []; // many distinct keys share slots: collisions must not merge them
for ($i = 0; $i < 3000; $i++) $wide[] = ["x$i" => 1, "y" . ($i * 7) => 2];
nocopy_put('w', $wide);
var_dump(nocopy_get('w') === $wide);

// keys in a copied value are shared strings; writing to it must not leak into others
$c = nocopy_get('a'); $c['r1']['id'] = -1; $c['r2']['new'] = 1;
var_dump(nocopy_get('a') === $rows);

$small = unserialize(serialize(array_slice($rows, 0, 40)));
nocopy_put('m', $small);
for ($i = 0; $i < 3000; $i++) { // churn: merges move 'm' (read often) forward
    nocopy_put("t$i", str_repeat('z', 2000));
    if ($i % 20 == 0) nocopy_get('m');
}
var_dump(nocopy_get('m') === $small, nocopy_stats()['evacuated_bytes'] > 0);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t034*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
