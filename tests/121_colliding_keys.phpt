--TEST--
nocopy: keys of one PHP hash (crafted: Ez/FY pairs) stay fast and correct: past a few, they take a secret alternative hash (were probed one by one under the shard lock: 60k took 10 s)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t121
nocopy.shm_size=128M
nocopy.entries_hint=100000
--FILE--
<?php
function k(int $i): string { $k = 'p:'; for ($b = 0; $b < 16; $b++) $k .= ($i >> $b) & 1 ? 'Ez' : 'FY'; return $k; }
$n = 40000;
$t = microtime(true);
for ($i = 0; $i < $n; $i++) nocopy_put(k($i), $i);
for ($i = 0; $i < $n; $i += 2) nocopy_put(k($i), "v$i"); // replaced where they are
$bad = 0;
for ($i = 0; $i < $n; $i++) $bad += nocopy_get(k($i)) !== ($i % 2 ? $i : "v$i");
for ($i = 0; $i < $n; $i++) $bad += nocopy_get(k($i) . 'x', $hit) !== null || $hit;
echo microtime(true) - $t < 2 ? "fast\n" : "slow\n";
echo "bad $bad, entries ", nocopy_stats()['entries'], ", listed ", count(nocopy_keys('p:')), "\n";
var_dump(nocopy_add(k(7), 1), nocopy_add(k($n), 'new'), nocopy_get(k($n)));
var_dump(nocopy_inc(k(9), 5), nocopy_cas(k(9), 14, 1), nocopy_get(k(9)));
for ($i = 0; $i < $n; $i += 3) $bad += !nocopy_delete(k($i));
for ($i = 0; $i < $n; $i++) $bad += nocopy_get(k($i), $hit) !== ($i % 3 ? ($i % 2 ? ($i == 9 ? 1 : $i) : "v$i") : null);
for ($i = 0; $i < $n; $i += 3) nocopy_put(k($i), -$i); // back, once each
for ($i = 0; $i < $n; $i += 3) $bad += nocopy_get(k($i)) !== -$i;
echo "bad $bad, entries ", nocopy_stats()['entries'], "\n";
var_dump(nocopy_remember(k(5) . 'r', fn () => 'a'), nocopy_remember(k(6) . 'r', fn () => 'b'));
nocopy_clear();
var_dump(nocopy_get(k(1)), nocopy_put(k(1), 1), nocopy_get(k(1)));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t121*')); ?>
--EXPECT--
fast
bad 0, entries 40000, listed 40000
bool(false)
bool(true)
string(3) "new"
int(14)
bool(true)
int(1)
bad 0, entries 40001
string(1) "a"
string(1) "b"
NULL
bool(true)
int(1)
