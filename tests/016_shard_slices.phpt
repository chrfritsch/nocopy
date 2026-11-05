--TEST--
nocopy: shards own fixed slices; an entry must fit in its shard's slice, and gets the shard's expired memory
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t016
apc.shm_size=4M
--FILE--
<?php
$s = nocopy_stats();
var_dump($s['shards']); // 8 MB slices at least: one shard here
for ($i = 0; nocopy_stats()['expunges'] == 0; $i++) nocopy_put("t$i", str_repeat('t', 2000), 1); // fill with short-lived data
sleep(2);
var_dump(nocopy_put('big', str_repeat('b', 3000000))); // most of the slice: needs the expired segments
var_dump(strlen(nocopy_get('big')));
var_dump(nocopy_put('huge', str_repeat('h', 5000000))); // more than the slice
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t016*')); ?>
--EXPECT--
int(1)
bool(true)
int(3000000)
bool(false)
