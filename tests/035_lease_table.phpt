--TEST--
stash: leases live outside the cache: invisible to iterators and counts, untouched by clear, released after use
--EXTENSIONS--
stash
--INI--
apc.enable_cli=1
--FILE--
<?php
$seen = stash_remember('a', function () {
    $in = [stash_stats()['leases'], stash_stats()['entries'], iterator_count(new APCUIterator()), count(apcu_cache_info()['cache_list'])];
    apcu_clear_cache();
    $in[] = stash_stats()['leases'];                                 // a clear does not drop a running compute
    $in[] = stash_remember('b', fn() => stash_stats()['leases']);   // nested: two held
    $in[] = stash_remember('a', fn() => 'inner');                    // recursion on its own key: no wait
    return $in;
});
var_dump($seen, stash_stats()['leases']);
try { apcu_entry('c', function () { throw new Exception('x'); }); } catch (Exception) {}
var_dump(stash_stats()['leases'], apcu_entry('c', fn() => 'ok'));
?>
--EXPECT--
array(7) {
  [0]=>
  int(1)
  [1]=>
  int(0)
  [2]=>
  int(0)
  [3]=>
  int(0)
  [4]=>
  int(1)
  [5]=>
  int(2)
  [6]=>
  string(5) "inner"
}
int(0)
int(0)
string(2) "ok"
