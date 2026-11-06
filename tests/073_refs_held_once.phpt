--TEST--
nocopy: a value whose references are each held once inside it (no graph) frees what sizing recorded about them
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t073
--FILE--
<?php
$r = array_fill(0, 40, 1);
$a = [];
foreach ($r as &$v) {
    $a[] = &$v; // held by $r too: once inside $a, so stored as its value
}
unset($v);
var_dump(nocopy_put('k', $a), nocopy_get('k') === array_fill(0, 40, 1));
$m = memory_get_usage();
for ($i = 0; $i < 500; $i++) {
    nocopy_put('k', $a);
}
var_dump(memory_get_usage() - $m < 4096);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t073*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
