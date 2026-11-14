--TEST--
nocopy: a value far larger built than in memory (arrays held many times) stops building at the largest entry, within memory_limit (grew without bound past it)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t120
nocopy.shm_size=32M
memory_limit=32M
--FILE--
<?php
// 2^30 arrays built, a few KB in memory: each level holds the one below twice, through references held once
$a = ['x' => str_repeat('y', 100)];
for ($i = 0; $i < 30; $i++) {
    $r1 = $a; $r2 = $a; $b = [];
    $b[0] = &$r1; $b[1] = &$r2;
    unset($r1, $r2);
    $a = $b; unset($b);
}
$t = microtime(true);
var_dump(nocopy_put('dag', $a), nocopy_add('dag', $a), nocopy_get('dag'));
$st = file_get_contents('/proc/self/status');
preg_match('/VmHWM:\s+(\d+)/', $st, $m);
echo $m[1] < 200 * 1024 ? "rss bounded\n" : "rss {$m[1]} kB\n";
echo microtime(true) - $t < 5 ? "fast\n" : "slow\n";
// what fits is still stored
$small = $a[0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0];
var_dump(nocopy_put('small', $small), nocopy_get('small') == $small);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t120*')); ?>
--EXPECT--
bool(false)
bool(false)
NULL
rss bounded
fast
bool(true)
bool(true)
