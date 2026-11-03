--TEST--
stash: worker mode (request_scope=0) fetches private copies and holds no pins
--EXTENSIONS--
stash
--INI--
stash.request_scope=0
--FILE--
<?php
$orig = ['name' => 'orig', 'list' => range(1, 300), 'nested' => ['a' => 'b']];
stash_put('k', $orig);
$held = stash_get('k');
ob_start(); debug_zval_dump($held); $d = ob_get_clean();
var_dump(str_contains($d, 'interned'));
for ($i = 0; $i < 300; $i++) stash_put('churn', range(1, 1000));
for ($i = 0; $i < 30; $i++) stash_put('k', array_fill(0, 200, "overwrite-$i")); // recycle freed blocks
$s = stash_stats();
var_dump($s['used'] < 300000, $s['pinned_segments'], $held === $orig);
var_dump(function_exists('stash_scope'), function_exists('stash_checkpoint'));
?>
--EXPECT--
bool(false)
bool(true)
int(0)
bool(true)
bool(false)
bool(false)
