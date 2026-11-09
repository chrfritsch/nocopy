--TEST--
nocopy: a value that is a reference (put_multiple over an array of references) starts its own key table: no string from the last value or from sizing leaks into it
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t088
nocopy.shm_size=2M
--FILE--
<?php
nocopy_put('prev', ['shared' => 'shared']); // fills the key table
$s = 'shared'; $f = 'fresh'; $a = ['x' => 'fresh', 'y' => 'shared'];
$vals = ['s' => &$s, 'f' => &$f, 'a' => &$a];
var_dump(nocopy_put_multiple($vals));
nocopy_delete('prev'); // ./dev asan: sizing and persisting disagreed (unreachable); without: strings pointing into 'prev' or garbage
var_dump(nocopy_get('s'), nocopy_get('f'), nocopy_get('a'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t088*')); ?>
--EXPECT--
bool(true)
string(6) "shared"
string(5) "fresh"
array(2) {
  ["x"]=>
  string(5) "fresh"
  ["y"]=>
  string(6) "shared"
}
