--TEST--
nocopy: a zero-copy request older than 3 s keeps nothing more: its further fetches copy (and stay correct)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t097
nocopy.shm_size=16M
--FILE--
<?php
require __DIR__ . '/serve.inc';
echo serve(<<<'CODE'
<?php
$v = fn($i) => str_repeat(chr(65 + $i % 26), 2000) . $i;
for ($i = 0; $i < 4000; $i++) nocopy_put("k$i", $v($i));
$held = []; $ok = true;
$read = function ($from, $to) use (&$held, &$ok, $v) {
    for ($i = $from; $i < $to; $i++) {
        $g = nocopy_get("k$i");
        $held[] = $g; $ok = $ok && $g === $v($i);
    }
};
$read(0, 300);
$young = nocopy_stats()['kept_segments'];
usleep(3200000);
$read(300, 4000);
$old = nocopy_stats()['kept_segments'];
var_dump($ok, $young > 0, $old <= $young + 1);
CODE);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t097*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
