--TEST--
nocopy: expired entries go away segment by segment without memory pressure, and do not fragment long-lived ones
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t014
apc.shm_size=4M
--FILE--
<?php
for ($i = 0; $i < 400; $i++) { // short-lived entries interleaved with permanent ones
    nocopy_put("perm$i", str_repeat('p', 1000));
    nocopy_put("tmp$i", str_repeat('t', 1000), 1);
}
$inuse = fn($s) => $s['segments'] - $s['free_segments'] - $s['dead_segments'];
$before = nocopy_stats();
sleep(2);
for ($i = 0; $i < 64; $i++) nocopy_put("tick$i", $i); // expiry is per shard, driven by writes: touch them all
$after = nocopy_stats();
var_dump($after['entries']);
var_dump($after['used'] < $before['used'] * 0.55);
var_dump($inuse($after) <= intdiv($inuse($before), 2) + 2);
var_dump(nocopy_get('perm399') === str_repeat('p', 1000), nocopy_get('tmp0'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t014*')); ?>
--EXPECT--
int(464)
bool(true)
bool(true)
bool(true)
NULL
