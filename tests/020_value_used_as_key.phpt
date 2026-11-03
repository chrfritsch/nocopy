--TEST--
stash: cached strings stored without a hash still work as array keys (zero-copy and copy mode)
--EXTENSIONS--
stash
--INI--
stash.request_scope=1
--FILE--
<?php
$k = str_repeat("ab", 300);
stash_put("x", [$k, "y", "z" => $k]);
$a = stash_get("x");
$m = [$a[0] => 1];
var_dump(isset($m[$k]), array_flip($a)["y"], array_search($k, $a, true), isset(array_count_values($a)[$k]));
var_dump(stash_scope(fn() => isset([stash_get("x")["z"] => 1][$k])));
?>
--EXPECT--
bool(true)
int(1)
int(0)
bool(true)
bool(true)
