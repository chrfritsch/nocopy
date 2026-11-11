--TEST--
nocopy.shm_name: 24 processes starting at once, over an arena its creator left unfinished, all end up in the same one
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (!is_dir('/dev/shm')) die('skip needs /dev/shm'); ?>
--INI--
nocopy.shm_name=/nocopy-t060-parent
--FILE--
<?php
require_once __DIR__ . '/php.inc';
$cmd = fn (string $code) => php_cmd(['nocopy.shm_name' => '/nocopy-t060'], ['-r', $code]) . ' 2>&1';
$probe = trim(shell_exec($cmd('echo nocopy_stats()["shm_name"];')));
$file = '/dev/shm/' . substr($probe, 1);
for ($round = 0; $round < 5; $round++) {
    unlink($file);
    touch($file); // what a creator that died before setting it up leaves
    chmod($file, 0600);
    $procs = [];
    for ($i = 0; $i < 24; $i++) {
        $procs[] = proc_open($cmd("usleep(random_int(0, 3000)); var_dump(nocopy_put('p$i', $i)); echo nocopy_shared_with();"), [1 => ['pipe', 'w']], $pipes[$i]);
    }
    $bad = 0;
    foreach ($procs as $i => $p) {
        $out = stream_get_contents($pipes[$i][1]);
        proc_close($p);
        $bad += $out !== "bool(true)\nhost";
    }
    $seen = trim(shell_exec($cmd('$n = 0; for ($i = 0; $i < 24; $i++) $n += nocopy_get("p$i") === $i; echo $n;')));
    echo "round $round: failed starts $bad, values seen $seen\n";
}
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t060*')); ?>
--EXPECT--
round 0: failed starts 0, values seen 24
round 1: failed starts 0, values seen 24
round 2: failed starts 0, values seen 24
round 3: failed starts 0, values seen 24
round 4: failed starts 0, values seen 24
