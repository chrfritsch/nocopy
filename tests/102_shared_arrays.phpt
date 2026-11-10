--TEST--
nocopy: arrays met in two writes are stored once arena-wide; only equal ones are shared, and blocks pointing at them survive merges
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t102
nocopy.shm_size=4M
--FILE--
<?php
$st = fn($k) => nocopy_stats()[$k];
$meta = ['contexts' => ['languages:language_interface', 'theme', 'user.permissions'], 'tags' => ['config:block_list'], 'max-age' => -1];

// first write: the doorkeeper sees it; second: a copy, then admitted; third: points at it
$u = $st('used'); nocopy_put('m1', ['#cache' => $meta, 'id' => 1]); $first = $st('used') - $u;
nocopy_put('m2', ['#cache' => $meta, 'id' => 2]);
$u = $st('used'); nocopy_put('m3', ['#cache' => $meta, 'id' => 3]); $third = $st('used') - $u;
var_dump($third < $first, $st('shared_arrays') >= 1, $st('shared_arrays_used') > 0);
var_dump(nocopy_get('m1')['#cache'] === $meta, nocopy_get('m3') === ['#cache' => $meta, 'id' => 3]);

// copy on write: the shared array stays as it was
$c = nocopy_get('m3'); $c['#cache']['tags'][] = 'x';
var_dump(nocopy_get('m3')['#cache'] === $meta);

// near misses are never shared: -0.0 and 0.0, '1' and 1, another key order
$pad = array_fill(0, 8, 'padding to pass the size threshold');
$twice = function ($k, $v) { nocopy_put("$k-a", $v); nocopy_put("$k-b", $v); nocopy_put("$k-c", $v); };
$twice('pz', ['x' => 0.0, 'p' => $pad]);
$twice('nz', ['x' => -0.0, 'p' => $pad]);
var_dump(fdiv(1, nocopy_get('nz-c')['x']), fdiv(1, nocopy_get('pz-c')['x']));
$twice('s', ['x' => '1', 'p' => $pad]);
$twice('i', ['x' => 1, 'p' => $pad]);
var_dump(nocopy_get('s-c')['x'], nocopy_get('i-c')['x']);
$twice('ab', ['a' => $pad, 'b' => 1]);
$twice('ba', ['b' => 1, 'a' => $pad]);
var_dump(array_keys(nocopy_get('ab-c')), array_keys(nocopy_get('ba-c')));

// an array holding an object is not shared; its inner arrays still are
$o = new ArrayObject([1]);
$twice('obj', ['o' => $o, 'm' => $meta]);
var_dump(nocopy_get('obj-c')['o'] == $o, nocopy_get('obj-c')['m'] === $meta);

// merges move read-often blocks that point at shared arrays forward: they read back intact
$keep = [];
for ($i = 0; $i < 40; $i++) {
    $keep["k$i"] = ['#cache' => $meta, 'i' => $i];
    nocopy_put("k$i", $keep["k$i"], 0);
}
$moved = $st('evacuated_bytes');
for ($i = 0; $i < 4000; $i++) {
    nocopy_put("t$i", str_repeat('z', 2000));
    if ($i % 100 == 0) foreach ($keep as $k => $_) nocopy_get($k);
}
$bad = 0;
foreach ($keep as $k => $v) $bad += nocopy_get($k) !== $v;
var_dump($st('evacuated_bytes') > $moved, $bad);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t102*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
float(-INF)
float(INF)
string(1) "1"
int(1)
array(2) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
}
array(2) {
  [0]=>
  string(1) "b"
  [1]=>
  string(1) "a"
}
bool(true)
bool(true)
bool(true)
int(0)
