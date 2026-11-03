--TEST--
stash: shards own fixed slices; an entry must fit in its shard's slice, and gets the shard's expired memory
--EXTENSIONS--
stash
--INI--
apc.shm_size=4M
--FILE--
<?php
$s = stash_stats();
var_dump($s['shards']); // 8 MB slices at least: one shard here
for ($i = 0; stash_stats()['expunges'] == 0; $i++) stash_put("t$i", str_repeat('t', 2000), 1); // fill with short-lived data
sleep(2);
var_dump(stash_put('big', str_repeat('b', 3000000))); // most of the slice: needs the expired segments
var_dump(strlen(stash_get('big')));
var_dump(stash_put('huge', str_repeat('h', 5000000))); // more than the slice
?>
--EXPECT--
int(1)
bool(true)
int(3000000)
bool(false)
