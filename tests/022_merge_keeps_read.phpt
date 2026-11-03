--TEST--
stash: a full cache evicts its oldest entries but keeps the ones read since they were written (merge eviction)
--EXTENSIONS--
stash
--INI--
apc.shm_size=8M
--FILE--
<?php
stash_put('hot', str_repeat('h', 500));
stash_put('cold', str_repeat('c', 500));
$v = str_repeat('x', 500);
$hot = 0;
for ($i = 0; $i < 100000; $i++) { // ~60M through 8M
    stash_put("k$i", $v);
    if ($i % 100 == 0) $hot += stash_get('hot') !== null;
}
$s = stash_stats();
var_dump($s['expunges'] > 50, $hot, stash_get('cold'), stash_get("k99999") === $v);
?>
--EXPECT--
bool(true)
int(1000)
NULL
bool(true)
