--TEST--
stash: worker mode (request_scope=0) is safe outside scopes, zero-copy inside, results are unshared
--EXTENSIONS--
stash
--INI--
stash.request_scope=0
--FILE--
<?php
$orig = ['name' => 'orig', 'list' => range(1, 300), 'nested' => ['a' => 'b']];
stash_put('k', $orig);

// outside a scope: a real copy, and no standing pin, so garbage is reclaimed without checkpoints
ob_start(); debug_zval_dump(stash_get('k')); $d = ob_get_clean();
var_dump(str_contains($d, 'interned'));
for ($i = 0; $i < 300; $i++) stash_put('churn', range(1, 1000));
$s = stash_stats();
var_dump($s['used'] < 300000, $s['pinned_segments']);

// inside a scope: zero-copy
stash_scope(function () {
    ob_start(); debug_zval_dump(stash_get('k')); $d = ob_get_clean();
    var_dump(str_contains($d, 'interned'));
});

// the result escapes safely, including values rebuilt with arena strings as keys
$out = stash_scope(function () {
    $o = [];
    foreach (stash_get('k') as $key => $v) $o[$key] = $v;
    return $o;
});
for ($i = 0; $i < 30; $i++) stash_put('k', array_fill(0, 200, "overwrite-$i")); // recycle freed blocks
var_dump($out === $orig);

// nesting: the inner exit must not unpin the outer scope
var_dump(stash_scope(function () use ($orig) {
    stash_put('k', $orig);
    $a = stash_get('k');
    stash_scope(fn() => 1);
    for ($i = 0; $i < 30; $i++) stash_put('k', array_fill(0, 200, "again-$i"));
    return [$a === $orig, stash_stats()['pinned_segments'] >= 1];
}));

// an exception leaves no pin behind
try { stash_scope(function () { stash_get('k'); throw new Exception('x'); }); } catch (Exception) {}
stash_put('k', [1]); stash_put('k', [2]);
var_dump(stash_stats()['pinned_segments']);
?>
--EXPECT--
bool(false)
bool(true)
int(0)
bool(true)
bool(true)
array(2) {
  [0]=>
  bool(true)
  [1]=>
  bool(true)
}
int(0)
