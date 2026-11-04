--TEST--
stash: a merge moves values by copying their bytes and repointing them: packed, nested, map and serialized values stay intact
--EXTENSIONS--
stash
--INI--
apc.shm_size=4M
stash.request_scope=0
--FILE--
<?php
class P { public $a = [1, 2]; }
$vals = [
    'packed' => [[1, 'x', [2.5, 'y']], range(1, 30), []],
    'map' => ['id' => 1, 'rows' => [['n' => 'a', 'v' => 'b'], ['n' => 'c', 'v' => 'd']]],
    'str' => str_repeat('s', 300),
    'obj' => new P,
];
foreach ($vals as $k => $v) stash_put($k, $v, 0, true);
$moved = stash_stats()['evacuated_bytes'];
for ($i = 0; $i < 4000; $i++) { // churn: merges move the read-often values forward
    stash_put("t$i", str_repeat('z', 2000));
    if ($i % 100 == 0) foreach ($vals as $k => $_) stash_get($k);
}
var_dump(stash_stats()['evacuated_bytes'] > $moved);
foreach ($vals as $k => $v) var_dump($k === 'obj' ? stash_get($k) == $v : stash_get($k) === $v);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
