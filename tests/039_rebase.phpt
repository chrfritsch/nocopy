--TEST--
nocopy: a merge moves values by copying their bytes and repointing them: packed, nested, map and serialized values stay intact
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t039
nocopy.shm_size=4M
nocopy.request_scope=0
--FILE--
<?php
class P { public $a = [1, 2]; }
$vals = [
    'packed' => [[1, 'x', [2.5, 'y']], range(1, 30), []],
    'map' => ['id' => 1, 'rows' => [['n' => 'a', 'v' => 'b'], ['n' => 'c', 'v' => 'd']]],
    'str' => str_repeat('s', 300),
    'obj' => new P,
];
foreach ($vals as $k => $v) nocopy_put($k, $v, 0, true);
$moved = nocopy_stats()['evacuated_bytes'];
for ($i = 0; $i < 4000; $i++) { // churn: merges move the read-often values forward
    nocopy_put("t$i", str_repeat('z', 2000));
    if ($i % 100 == 0) foreach ($vals as $k => $_) nocopy_get($k);
}
var_dump(nocopy_stats()['evacuated_bytes'] > $moved);
foreach ($vals as $k => $v) var_dump($k === 'obj' ? nocopy_get($k) == $v : nocopy_get($k) === $v);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t039*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
