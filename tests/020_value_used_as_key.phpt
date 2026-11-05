--TEST--
nocopy: cached strings stored without a hash still work as array keys (zero-copy and copy mode)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t020
nocopy.request_scope=1
--FILE--
<?php
$k = str_repeat("ab", 300);
nocopy_put("x", [$k, "y", "z" => $k]);
$a = nocopy_get("x");
$m = [$a[0] => 1];
var_dump(isset($m[$k]), array_flip($a)["y"], array_search($k, $a, true), isset(array_count_values($a)[$k]));
var_dump(isset([nocopy_get("x")["z"] => 1][$k]));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t020*')); ?>
--EXPECT--
bool(true)
int(1)
int(0)
bool(true)
bool(true)
