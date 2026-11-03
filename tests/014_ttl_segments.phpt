--TEST--
stash: expired entries go away segment by segment without memory pressure, and do not fragment long-lived ones
--EXTENSIONS--
stash
--INI--
apc.shm_size=4M
--FILE--
<?php
for ($i = 0; $i < 400; $i++) { // short-lived entries interleaved with permanent ones
    stash_put("perm$i", str_repeat('p', 1000));
    stash_put("tmp$i", str_repeat('t', 1000), 1);
}
$inuse = fn($s) => $s['segments'] - $s['free_segments'] - $s['dead_segments'];
$before = stash_stats();
sleep(2);
for ($i = 0; $i < 64; $i++) stash_put("tick$i", $i); // expiry is per shard, driven by writes: touch them all
$after = stash_stats();
var_dump($after['entries']);
var_dump($after['used'] < $before['used'] * 0.55);
var_dump($inuse($after) <= intdiv($inuse($before), 2) + 2);
var_dump(stash_get('perm399') === str_repeat('p', 1000), stash_get('tmp0'));
?>
--EXPECT--
int(464)
bool(true)
bool(true)
bool(true)
NULL
