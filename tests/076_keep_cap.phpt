--TEST--
nocopy: a zero-copy request keeps at most a quarter of the segments pinned; past that its fetches copy (and stay correct)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t076
nocopy.shm_size=4M
--FILE--
<?php
require __DIR__ . '/serve.inc';
echo serve(<<<'CODE'
<?php
$v = fn($i) => str_repeat(chr(65 + $i % 26), 2000) . $i;
for ($i = 0; $i < 1200; $i++) nocopy_put("k$i", $v($i)); // spread over every segment
$held = [];
$ok = true;
for ($i = 0; $i < 1200; $i++) {
    $g = nocopy_get("k$i", $hit);
    if ($hit) { $held[$i] = $g; $ok = $ok && $g === $v($i); }
}
for ($i = 0; $i < 3000; $i++) nocopy_put("w$i", $v($i)); // churn: only the kept pins can block reclaim
foreach ($held as $i => $g) $ok = $ok && $g === $v($i);
$s = nocopy_stats();
var_dump($ok, count($held) > 100, $s['pinned_segments'] <= intdiv($s['segments'], 4) + 1, $s['entries'] > 600);
CODE);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t076*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
