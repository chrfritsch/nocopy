--TEST--
stash: stash_put(..., serialize: true) stores objects serialized
--EXTENSIONS--
stash
--FILE--
<?php
$o = new ArrayObject(['a' => 1]);
var_dump(stash_put('o', ['obj' => $o], 0, true));
$v = stash_get('o', $hit);
var_dump($hit, $v['obj'] == $o, $v['obj'] !== $o);
try { stash_put('p', $o); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
stash_put(): Argument #2 ($value) must not contain objects or resources
