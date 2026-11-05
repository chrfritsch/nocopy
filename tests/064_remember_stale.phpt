--TEST--
nocopy: a nocopy_remember waiter gets the expired value while another worker recomputes, only up to one TTL past expiry; never with beta 0 (apcu_entry)
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t064
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
function race(string $k): array {
    $p = pcntl_fork();
    if ($p === 0) { nocopy_remember($k, function () { usleep(800000); return 'new'; }, 2); exit; }
    usleep(200000); // the child holds the lease
    $t = microtime(true);
    $v = nocopy_remember($k, fn() => 'mine', 2);
    $fast = microtime(true) - $t < 0.4;
    pcntl_waitpid($p, $st);
    return [$v, $fast];
}

nocopy_put('a', 'old', 2);
sleep(3); // expired, within one TTL
var_dump(race('a'), nocopy_get('a'));

nocopy_put('b', 'old', 1);
sleep(3); // more than one TTL past expiry: wait for the holder
var_dump(race('b'));

nocopy_put('c', 'old', 2);
sleep(3);
$p = pcntl_fork();
if ($p === 0) { nocopy_remember('c', function () { usleep(500000); return 'new'; }, 2); exit; }
usleep(200000);
var_dump(apcu_entry('c', fn() => 'mine', 2)); // beta 0: no stale value
pcntl_waitpid($p, $st);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t064*')); ?>
--EXPECT--
array(2) {
  [0]=>
  string(3) "old"
  [1]=>
  bool(true)
}
string(3) "new"
array(2) {
  [0]=>
  string(3) "new"
  [1]=>
  bool(false)
}
string(3) "new"
