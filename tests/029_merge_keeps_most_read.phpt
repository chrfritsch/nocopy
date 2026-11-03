--TEST--
stash: when the read entries of a merge do not all fit, the most-read ones survive
--EXTENSIONS--
stash
--INI--
apc.shm_size=4M
--FILE--
<?php
$v = str_repeat('x', 500);
for ($i = 0; $i < 6000; $i++) { // ~3.6M: fills the arena; 20 keys spread through it
    stash_put("a$i", $v);
    if ($i % 300 == 150) stash_put("f$i", $v);
}
for ($i = 0; $i < 6000; $i++) stash_get("a$i");
for ($r = 0; $r < 100; $r++) for ($i = 150; $i < 6000; $i += 300) stash_get("f$i");
for ($i = 0; $i < 3000; $i++) stash_put("b$i", $v); // merges of segments full of read entries: not all fit
$f = 0;
for ($i = 150; $i < 6000; $i += 300) $f += stash_get("f$i") === $v;
var_dump(stash_stats()['evicted'] > 1000, $f); // the single read bit kept 11 of 20
?>
--EXPECT--
bool(true)
int(20)
