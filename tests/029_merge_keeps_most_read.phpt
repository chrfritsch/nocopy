--TEST--
nocopy: when the read entries of a merge do not all fit, the most-read ones survive
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t029
nocopy.shm_size=4M
--FILE--
<?php
$v = str_repeat('x', 500);
for ($i = 0; $i < 6000; $i++) { // ~3.6M: fills the arena; 20 keys spread through it
    nocopy_put("a$i", $v);
    if ($i % 300 == 150) nocopy_put("f$i", $v);
}
for ($i = 0; $i < 6000; $i++) nocopy_get("a$i");
for ($r = 0; $r < 100; $r++) for ($i = 150; $i < 6000; $i += 300) nocopy_get("f$i");
for ($i = 0; $i < 3000; $i++) nocopy_put("b$i", $v); // merges of segments full of read entries: not all fit
$f = 0;
for ($i = 150; $i < 6000; $i += 300) $f += nocopy_get("f$i") === $v;
var_dump(nocopy_stats()['evicted'] > 1000, $f); // the single read bit kept 11 of 20
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t029*')); ?>
--EXPECT--
bool(true)
int(20)
