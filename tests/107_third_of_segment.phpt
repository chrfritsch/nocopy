--TEST--
nocopy: a segment without dead bytes is not compacted alone (two values of a third of a segment each): a full arena evicts instead of copying them in circles
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t107
nocopy.shm_size=4M
nocopy.entries_hint=4096
--FILE--
<?php
$v = str_repeat('x', intdiv(nocopy_stats()['segment_size'], 3)); // two per segment, 2/3 live, nothing dead
$failed = 0;
for ($i = 0; $i < 600; $i++) {
    $failed += !nocopy_put("k$i", $v); // more keys than the arena holds
}
$s = nocopy_stats();
var_dump($failed, $s['evicted'] > 0, $s['evacuated_bytes'] < 600 * strlen($v));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t107*')); ?>
--EXPECT--
int(0)
bool(true)
bool(true)
