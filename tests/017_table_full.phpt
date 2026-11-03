--TEST--
stash: the slot table is fixed; when entries outnumber it the oldest are evicted, and apc.entries_hint sizes it
--EXTENSIONS--
stash
--INI--
apc.shm_size=8M
--FILE--
<?php
$n = 40000; // ~96 bytes each: fits the memory, not the 32k slots (24k usable)
for ($i = 0; $i < $n; $i++) stash_put("k$i", $i);
$recent = 0;
for ($i = $n - 1000; $i < $n; $i++) $recent += stash_get("k$i") === $i;
$s = stash_stats();
var_dump($s['slots'], $s['entries'] < $n, $s['expunges'] > 0, $recent, stash_get('k0'));
?>
--EXPECT--
int(32768)
bool(true)
bool(true)
int(1000)
NULL
