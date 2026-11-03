--TEST--
stash: the slot table grows when small entries outnumber its initial slots
--EXTENSIONS--
stash
--INI--
apc.shm_size=8M
--FILE--
<?php
$n = 40000; // ~96 bytes each: more than the initial 32k slots
$ok = 0;
for ($i = 0; $i < $n; $i++) $ok += stash_put("k$i", $i);
$bad = 0;
for ($i = 0; $i < $n; $i++) $bad += stash_get("k$i") !== $i;
$s = stash_stats();
var_dump($ok, $bad, $s['entries'], $s['slots'] > $n);
?>
--EXPECT--
int(40000)
int(0)
int(40000)
bool(true)
