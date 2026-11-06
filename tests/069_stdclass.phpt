--TEST--
nocopy: stdClass and dynamic properties as graph nodes: same values as unserialize(), every way of writing a fetched object leaves the cache intact
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t069
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
#[AllowDynamicProperties] class Dyn { public $a = 1; private $p = 'p'; }
CODE;
$test = <<<'CODE'
$json = json_decode('{"id":1,"name":"n","tags":["a","b"],"child":{"x":1.5,"list":[{"k":"v"},{"k":"w"}]},"0":"zero","":"empty"}');
$d = new Dyn; $d->b = ['x' => 1]; $d->o = $json->child;
$same = new stdClass; $same->v = 'shared';
$vals = [
    'json' => $json,
    'dyn' => $d,
    'list' => [$same, $same, (object) [], (object) ['n' => null]],
];
foreach ($vals as $k => $v) {
    nocopy_put($k, $v);
    for ($i = 0; $i < 3; $i++) {
        if (serialize(nocopy_get($k)) !== serialize($v)) echo "$k #$i DIFFERS\n";
    }
}
$l = nocopy_get('list');
var_dump($l[0] === $l[1], get_class($l[2]), count((array) $l[2]), property_exists($l[3], 'n'));
$want = serialize($json);
for ($i = 0; $i < 3; $i++) {
    $o = nocopy_get('json');
    $o->name = 'changed';
    $o->new = 1;
    unset($o->id);
    $o->tags[] = 'c';
    $o->child->x++;
    $o->child->list[0]->k = 'changed';
    $r = &$o->child->list; $r[] = 'by ref'; unset($r);
    foreach ($o as $name => &$val) { if (is_string($val)) $val = "$val!"; } unset($val);
    $ao = @new ArrayObject($o->child); $ao['ao'] = 1; $ao['x'] = 0;
    $c = clone $o; $c->cloned = true;
    $arr = (array) $o; $arr['cast'] = 1;
    $vars = get_object_vars($o);
    $o->{'0'} = 'num';
}
var_dump(serialize(nocopy_get('json')) === $want);
var_dump($o->name, isset($o->id), $o->tags, $o->child->x, $o->child->ao, $o->{'0'}, $o->{''}, count($o->child->list));
$dd = nocopy_get('dyn');
$dd->b['y'] = 2; $dd->a = 2; $dd->o->x = 9;
var_dump(serialize(nocopy_get('dyn')) === serialize($d), array_keys(get_object_vars(nocopy_get('dyn'))));
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t069*')); ?>
--EXPECT--
-- copy mode
bool(true)
string(8) "stdClass"
int(0)
bool(true)
bool(true)
string(8) "changed!"
bool(false)
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
  [2]=>
  string(1) "c"
}
int(0)
int(1)
string(3) "num"
string(6) "empty!"
int(3)
bool(true)
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
  [2]=>
  string(1) "o"
}
-- zero-copy
bool(true)
string(8) "stdClass"
int(0)
bool(true)
bool(true)
string(8) "changed!"
bool(false)
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
  [2]=>
  string(1) "c"
}
int(0)
int(1)
string(3) "num"
string(6) "empty!"
int(3)
bool(true)
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
  [2]=>
  string(1) "o"
}
