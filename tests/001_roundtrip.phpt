--TEST--
nocopy: values round-trip and arrays come back immutable
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t001
nocopy.request_scope=1
--FILE--
<?php
$v = ['a' => 1, 'b' => [1, 2.5, 'x', null, true], 3 => 'str'];
var_dump(nocopy_put('k', $v));
var_dump(nocopy_get('k', $hit) === $v, $hit);
var_dump(nocopy_get('missing', $hit), $hit);
nocopy_put('s', 'hello');
var_dump(nocopy_get('s'));
nocopy_put('e', []);
var_dump(nocopy_get('e'));
$r = ['x' => 1]; $ref = &$r['x'];
nocopy_put('r', $r);
var_dump(nocopy_get('r'));
// shows "interned": the array lives in shared memory, no refcounting
debug_zval_dump(nocopy_get('k'));
// writing separates, the stored copy is untouched
$a = nocopy_get('k'); $a['a'] = 99;
var_dump(nocopy_get('k')['a']);
try { nocopy_put('o', new stdClass); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t001*')); ?>
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
  array(5) %Sinterned {
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
nocopy_put(): Argument #2 ($value) must not contain objects or resources
