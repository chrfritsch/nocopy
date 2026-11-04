--TEST--
nocopy_get_multiple / nocopy_put_multiple, and the apcu_fetch / apcu_store array forms on top of them
--EXTENSIONS--
nocopy
--INI--
apc.enable_cli=1
--FILE--
<?php
var_dump(nocopy_put_multiple(['a' => [1], 'b' => 'x', 7 => 2.5], 100));
var_dump(nocopy_get_multiple(['a', 'missing', 'b', '7', 5]));
try {
    nocopy_put_multiple(['p' => 1, 'o' => new stdClass, 'q' => 2]);
} catch (TypeError $e) {
    echo get_class($e), "\n";
}
var_dump(nocopy_get('p'), nocopy_get('q', $hit), $hit);
var_dump(nocopy_put_multiple(['o' => new ArrayObject([1])], 0, true), nocopy_get('o') instanceof ArrayObject);
var_dump(apcu_store(['c' => 1, 'd' => 2]), apcu_fetch(['c', 'd', 'nope']));
?>
--EXPECT--
bool(true)
array(3) {
  ["a"]=>
  array(1) {
    [0]=>
    int(1)
  }
  ["b"]=>
  string(1) "x"
  [7]=>
  float(2.5)
}
TypeError
int(1)
NULL
bool(false)
bool(true)
bool(true)
array(0) {
}
array(2) {
  ["c"]=>
  int(1)
  ["d"]=>
  int(2)
}
