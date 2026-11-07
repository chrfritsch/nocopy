--TEST--
nocopy: entries over half a segment are not compacted (moving one takes a whole segment again): a full arena evicts instead of copying them in circles
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t077
nocopy.shm_size=4M
nocopy.entries_hint=4096
--FILE--
<?php
$seg = nocopy_stats()['segment_size'];
mt_srand(7);
$written = $failed = 0;
for ($i = 0; $i < 600; $i++) {
    $v = str_repeat('x', mt_rand($seg / 2 + 1000, $seg - 1000)); // one per segment
    $failed += !nocopy_put('k' . mt_rand(0, 99), $v);            // 100 keys: more than the arena holds
    $written += strlen($v);
}
$s = nocopy_stats();
var_dump($failed, $s['evicted'] > 0, $s['evacuated_bytes'] < $written);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t077*')); ?>
--EXPECT--
int(0)
bool(true)
bool(true)
