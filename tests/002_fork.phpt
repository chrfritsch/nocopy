--TEST--
nocopy: forked workers share one arena, concurrent writers do not lose keys
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t002
--FILE--
<?php
$pids = [];
foreach (range(0, 3) as $w) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        for ($i = 0; $i < 500; $i++) nocopy_put("w$w:$i", [$w, $i]);
        exit(0);
    }
    $pids[] = $pid;
}
foreach ($pids as $p) pcntl_waitpid($p, $st);
$missing = 0;
foreach (range(0, 3) as $w) for ($i = 0; $i < 500; $i++) {
    if (nocopy_get("w$w:$i") !== [$w, $i]) $missing++;
}
var_dump($missing);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t002*')); ?>
--EXPECT--
int(0)
