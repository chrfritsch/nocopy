--TEST--
stash: values round-trip and arrays come back immutable
--EXTENSIONS--
stash
--INI--
stash.request_scope=1
--FILE--
<?php
$v = ['a' => 1, 'b' => [1, 2.5, 'x', null, true], 3 => 'str'];
var_dump(stash_put('k', $v));
var_dump(stash_get('k', $hit) === $v, $hit);
var_dump(stash_get('missing', $hit), $hit);
stash_put('s', 'hello');
var_dump(stash_get('s'));
stash_put('e', []);
var_dump(stash_get('e'));
$r = ['x' => 1]; $ref = &$r['x'];
stash_put('r', $r);
var_dump(stash_get('r'));
// shows "interned": the array lives in shared memory, no refcounting
debug_zval_dump(stash_get('k'));
// writing separates, the stored copy is untouched
$a = stash_get('k'); $a['a'] = 99;
var_dump(stash_get('k')['a']);
try { stash_put('o', new stdClass); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
NULL
bool(false)
string(5) "hello"
array(0) {
}
array(1) {
  ["x"]=>
  int(1)
}
array(3) interned {
  ["a"]=>
  int(1)
  ["b"]=>
  array(5) packed interned {
    [0]=>
    int(1)
    [1]=>
    float(2.5)
    [2]=>
    string(1) "x" interned
    [3]=>
    NULL
    [4]=>
    bool(true)
  }
  [3]=>
  string(3) "str" interned
}
int(1)
stash_put(): Argument #2 ($value) must not contain objects or resources
