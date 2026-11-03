--TEST--
stash: a write that needs memory held by other shards' expired data gets it
--EXTENSIONS--
stash
--INI--
apc.shm_size=4M
--FILE--
<?php
var_dump(stash_stats()['shards'] > 1);
for ($i = 0; stash_put("t$i", str_repeat('t', 2000), 1) && stash_get("t0") !== null; $i++); // fill every shard with short-lived data
sleep(2);
var_dump(stash_put('big', str_repeat('b', 2200000))); // one key, one shard: more than its own expired segments hold
var_dump(strlen(stash_get('big')));
?>
--EXPECT--
bool(true)
bool(true)
int(2200000)
