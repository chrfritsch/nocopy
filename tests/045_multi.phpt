--TEST--
nocopy_get_multiple / nocopy_put_multiple, and the apcu_fetch / apcu_store array forms on top of them
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t045
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
var_dump(nocopy_put_multiple(['a' => [1], 'b' => 'x', 7 => 2.5], 100));
var_dump(nocopy_get_multiple(['a', 'missing', 'b', '7', 5]));
var_dump(nocopy_put_multiple(['p' => 1, 'f' => STDIN, 'q' => 2]));
var_dump(nocopy_get('p'), nocopy_get('q', $hit), $hit);
var_dump(nocopy_put_multiple(['o' => new ArrayObject([1])], 0), nocopy_get('o') instanceof ArrayObject);
var_dump(apcu_store(['c' => 1, 'd' => 2]), apcu_fetch(['c', 'd', 'nope']));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t045*')); ?>
--EXPECTF--
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

Warning: nocopy_put_multiple(): nocopy: cannot store resources in %s on line %d
bool(false)
int(1)
int(2)
bool(true)
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
