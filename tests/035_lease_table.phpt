--TEST--
nocopy: leases live outside the cache: invisible to iterators and counts, untouched by clear, released after use
--EXTENSIONS--
nocopy
--INI--
apc.enable_cli=1
--FILE--
<?php
$seen = nocopy_remember('a', function () {
    $in = [nocopy_stats()['leases'], nocopy_stats()['entries'], iterator_count(new APCUIterator()), count(apcu_cache_info()['cache_list'])];
    apcu_clear_cache();
    $in[] = nocopy_stats()['leases'];                                 // a clear does not drop a running compute
    $in[] = nocopy_remember('b', fn() => nocopy_stats()['leases']);   // nested: two held
    $in[] = nocopy_remember('a', fn() => 'inner');                    // recursion on its own key: no wait
    return $in;
});
var_dump($seen, nocopy_stats()['leases']);
try { apcu_entry('c', function () { throw new Exception('x'); }); } catch (Exception) {}
var_dump(nocopy_stats()['leases'], apcu_entry('c', fn() => 'ok'));
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
