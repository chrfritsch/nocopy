--TEST--
nocopy: a deleted value's open segment in a TTL bucket nobody writes to again does not block shared strings' rotation forever
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t115
nocopy.shm_size=4M
--FILE--
<?php
function strs($tag, $n) { $a = []; for ($i = 0; $i < $n; $i++) $a[] = "$tag-string-number-$i"; return $a; }
for ($r = 0; $r < 3; $r++) nocopy_put('once', strs('once', 50), 1000000); // the only write in its TTL bucket
nocopy_delete('once');
$t = microtime(true);
for ($round = 0; nocopy_stats()['shared_strings_rotations'] < 4 && microtime(true) - $t < 15; $round++) {
    for ($k = 0; $k < 20; $k++) for ($r = 0; $r < 2; $r++) nocopy_put("k$k", strs("r$round-k$k", 100));
    usleep(100000);
}
var_dump(nocopy_stats()['shared_strings_rotations'] >= 4);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t115*')); ?>
--EXPECT--
bool(true)
