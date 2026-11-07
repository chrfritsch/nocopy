--TEST--
nocopy_keys: live keys (oldest first, to the clock tick), by prefix, with size, created and expires
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t078
--FILE--
<?php
for ($i = 0; $i < 1000; $i++) {
    nocopy_put("k.$i", str_repeat('x', $i));
}
nocopy_put('other', 1, 60);
nocopy_put('42', 'numeric');
nocopy_delete('k.5');

$all = nocopy_keys();
var_dump(count($all));
$want = array_merge(array_map(fn ($i) => "k.$i", array_diff(range(0, 999), [5])), ['other', 42]);
var_dump(array_diff($want, array_keys($all)), isset($all[42]));
$p = array_keys(nocopy_keys('k.99'));
sort($p);
var_dump(count(nocopy_keys('k.')), $p, nocopy_keys('nope'));

$o = $all['other'];
var_dump($o['expires'] - $o['created'], abs($o['created'] - time()) <= 1, $all['k.0']['expires'], $all['k.999']['size'] > $all['k.0']['size']);

nocopy_clear();
var_dump(nocopy_keys());
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t078*')); ?>
--EXPECT--
int(1001)
array(0) {
}
bool(true)
int(999)
array(11) {
  [0]=>
  string(4) "k.99"
  [1]=>
  string(5) "k.990"
  [2]=>
  string(5) "k.991"
  [3]=>
  string(5) "k.992"
  [4]=>
  string(5) "k.993"
  [5]=>
  string(5) "k.994"
  [6]=>
  string(5) "k.995"
  [7]=>
  string(5) "k.996"
  [8]=>
  string(5) "k.997"
  [9]=>
  string(5) "k.998"
  [10]=>
  string(5) "k.999"
}
array(0) {
}
int(60)
bool(true)
int(0)
bool(true)
array(0) {
}
