--TEST--
nocopy: leases live outside the cache: invisible to entry counts, untouched by clear, released after use
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t035
--FILE--
<?php
$seen = nocopy_remember('a', function () {
    $in = [nocopy_stats()['leases'], nocopy_stats()['entries']];
    nocopy_clear();
    $in[] = nocopy_stats()['leases'];                                 // a clear does not drop a running compute
    $in[] = nocopy_remember('b', fn() => nocopy_stats()['leases']);   // nested: two held
    $in[] = nocopy_remember('a', fn() => 'inner');                    // recursion on its own key: no wait
    return $in;
});
var_dump($seen, nocopy_stats()['leases']);
try { nocopy_remember('c', function () { throw new Exception('x'); }); } catch (Exception) {}
var_dump(nocopy_stats()['leases'], nocopy_remember('c', fn() => 'ok'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t035*')); ?>
--EXPECT--
array(5) {
  [0]=>
  int(1)
  [1]=>
  int(0)
  [2]=>
  int(1)
  [3]=>
  int(2)
  [4]=>
  string(5) "inner"
}
int(0)
int(0)
string(2) "ok"
