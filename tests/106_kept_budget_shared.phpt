--TEST--
nocopy: kept copies of entries pointing at shared arrays count their copy's bytes against nocopy.local_size
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t106
nocopy.shm_size=16M
nocopy.local_size=1M
--FILE--
<?php
$rows = [];
for ($i = 0; $i < 200; $i++) $rows["key$i"] = ['id' => $i, 'name' => "name $i", 'tags' => ['a', 'b'], 'score' => $i * 1.5];
for ($i = 0; $i < 300; $i++) nocopy_put("k$i", $rows); // shared from the third store on: tiny entries, big copies
$m = memory_get_usage();
for ($r = 0; $r < 3; $r++) for ($i = 0; $i < 300; $i++) $x = nocopy_get("k$i");
var_dump($x === $rows, memory_get_usage() - $m < 4 << 20);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t106.*')); ?>
--EXPECT--
bool(true)
bool(true)
