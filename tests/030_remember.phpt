--TEST--
stash: stash_remember() computes once, releases its lease on exceptions and recursion, refreshes early only by XFetch
--EXTENSIONS--
stash
--FILE--
<?php
$n = 0;
$f = function ($k) use (&$n) { $n++; return "$k$n"; };
var_dump(stash_remember('a', $f), stash_remember('a', $f), $n);

try { stash_remember('x', function () { throw new Exception('boom'); }); } catch (Exception $e) { echo $e->getMessage(), "\n"; }
$t = microtime(true);
var_dump(stash_remember('x', fn() => 'ok'), microtime(true) - $t < 1); // no lease left behind

$t = microtime(true);
var_dump(stash_remember('r', fn() => stash_remember('r', fn() => 'inner') . '+outer'), microtime(true) - $t < 1);

$n = 0;
$slow = function () use (&$n) { usleep(20000); return ++$n; };
stash_remember('e', $slow, 100);
var_dump(stash_remember('e', $slow, 100), stash_remember('e', $slow, 100, 0.0)); // 20 ms vs 100 s left: never early
var_dump(stash_remember('e', $slow, 100, 1e9), stash_get('e')); // a huge beta always refreshes
?>
--EXPECT--
string(2) "a1"
string(2) "a1"
int(1)
boom
string(2) "ok"
bool(true)
string(11) "inner+outer"
bool(true)
int(1)
int(1)
int(2)
int(2)
