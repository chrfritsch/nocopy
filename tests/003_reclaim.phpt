--TEST--
nocopy: replaced entries are compacted away when the arena is under pressure
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t003
apc.shm_size=2M
--FILE--
<?php
$big = range(1, 1000);
$ok = 0;
for ($i = 0; $i < 600; $i++) {       // ~10 MB written into a 2 MB arena
    $ok += nocopy_put('k', $big);
    nocopy_put("s$i", $i);            // small neighbours keep every segment partly live
    if ($i >= 50) nocopy_delete("s" . ($i - 50));
}
$s = nocopy_stats();
var_dump($ok, $s['entries'], $s['used'] < 500000, nocopy_get('k') === $big, nocopy_get('s599'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t003*')); ?>
--EXPECT--
int(600)
int(51)
bool(true)
bool(true)
int(599)
