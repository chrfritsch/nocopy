--TEST--
nocopy: shared references as graph nodes (arrays, typed and untyped slots, cycles): same shape as unserialize(), writes through them stay private
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t070
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
class T { public int $n = 1; public $u = 'u'; public ?array $list = null; }
CODE;
$test = <<<'CODE'
$a = ['x' => [1, 2], 'name' => 'n'];
$a['y'] = &$a['x'];
$t = new T; $t->list = [1]; $t->u = &$t->list; $other = new T; $other->n = &$t->n;
$c = []; $c[0] = &$c;
$once = ['k' => 1]; $r = &$once['k'];
$vals = ['arr' => $a, 'obj' => [$t, $other], 'cycle' => $c, 'once' => $once, 'mixed' => ['s' => str_repeat('z', 50), 'r1' => &$a['name'], 'r2' => &$a['name']]];
foreach ($vals as $k => $v) {
    nocopy_put($k, $v);
    for ($i = 0; $i < 3; $i++) {
        if (serialize(nocopy_get($k)) !== serialize($v)) echo "$k #$i DIFFERS\n";
    }
}
for ($i = 0; $i < 3; $i++) {
    $g = nocopy_get('arr');
    $g['y'][] = 3;
    $m = nocopy_get('mixed');
    $m['r1'] = 'changed';
    $o = nocopy_get('obj');
    $o[1]->n = 5;
    try { $o[0]->n = 'no'; } catch (TypeError $e) { $err = get_class($e); }
    $o[0]->u = [7];
}
var_dump($g['x'], $m['r2'], $o[0]->n, $err, $o[0]->list);
var_dump(serialize(nocopy_get('arr')) === serialize($a), serialize(nocopy_get('obj')) === serialize([$t, $other]));
$cy = nocopy_get('cycle');
var_dump(count($cy[0][0][0]));
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t070*')); ?>
--EXPECT--
-- copy mode
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
}
string(7) "changed"
int(5)
string(9) "TypeError"
array(1) {
  [0]=>
  int(7)
}
bool(true)
bool(true)
int(1)
-- zero-copy
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
}
string(7) "changed"
int(5)
string(9) "TypeError"
array(1) {
  [0]=>
  int(7)
}
bool(true)
bool(true)
int(1)
