--TEST--
nocopy: the pin budget is arena-wide: concurrent zero-copy requests together keep about a quarter of the segments (design review 2026-11, finding 2)
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t096
nocopy.shm_size=16M
--FILE--
<?php
require __DIR__ . '/serve.inc';
$v = fn($i) => str_repeat(chr(65 + $i % 26), 2000) . $i;
for ($i = 0; $i < 6000; $i++) nocopy_put("k$i", $v($i)); // over most segments
$n = 4;
$kids = [];
for ($j = 0; $j < $n; $j++) {
    if (($p = pcntl_fork()) === 0) {
        // each request reads its own quarter first: without a shared budget each keeps a quarter of its own
        $out = serve(<<<CODE
<?php
\$held = []; \$ok = true;
for (\$k = 0; \$k < 6000; \$k++) {
    \$i = (\$k + $j * 1500) % 6000;
    \$g = nocopy_get("k\$i", \$hit);
    if (\$hit) { \$held[] = \$g; \$ok = \$ok && \$g === str_repeat(chr(65 + \$i % 26), 2000) . \$i; }
}
nocopy_put("ready$j", 1);
while (nocopy_get('done') === null) usleep(10000); // a slow backend: the request holds what it kept
echo \$ok ? 'ok' : 'bad';
CODE);
        exit($out === 'ok' ? 0 : 1);
    }
    $kids[] = $p;
}
for ($t = 0; $t < 3000; $t++) {
    $ready = 0;
    for ($j = 0; $j < $n; $j++) $ready += nocopy_get("ready$j") !== null;
    if ($ready === $n) break;
    usleep(10000);
}
$s = nocopy_stats();
echo "ready: $ready\n";
// a quarter, plus up to 7 per request between two looks at the union
var_dump($s['kept_segments'] > 0, $s['kept_segments'] <= intdiv($s['segments'], 4) + 8 * $n);
nocopy_put('done', 1);
$good = 0;
foreach ($kids as $p) { pcntl_waitpid($p, $st); $good += pcntl_wexitstatus($st) === 0; }
echo "requests with correct values: $good\n";
var_dump(nocopy_stats()['kept_segments']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t096*')); ?>
--EXPECT--
ready: 4
bool(true)
bool(true)
requests with correct values: 4
int(0)
