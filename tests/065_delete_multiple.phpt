--TEST--
nocopy_delete_multiple: removes the given keys, counts those that existed, skips non-strings
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t065
--FILE--
<?php
nocopy_put_multiple(['a' => 1, 'b' => [2], 'c' => 'x', '7' => 7]);
var_dump(nocopy_delete_multiple(['a', 'missing', 'b', 7]));
var_dump(nocopy_get_multiple(['a', 'b', 'c', '7']));
var_dump(nocopy_delete_multiple([]));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t065*')); ?>
--EXPECT--
int(2)
array(2) {
  ["c"]=>
  string(1) "x"
  [7]=>
  int(7)
}
int(0)
