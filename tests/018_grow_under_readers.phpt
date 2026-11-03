--TEST--
stash: readers never miss or misread keys while writers grow every shard's table
--EXTENSIONS--
stash
pcntl
--INI--
apc.shm_size=32M
--FILE--
<?php
for ($i = 0; $i < 200; $i++) stash_put("f$i", $i % 2 ? $i : ["n" => $i, "s" => "v$i"]);
$before = stash_stats()['slots'];
$pids = [];
for ($w = 0; $w < 8; $w++) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        $bad = 0;
        if ($w < 4) { // writer: 40k unique keys
            for ($i = 0; $i < 40000; $i++) $bad += !stash_put("w{$w}_$i", $i);
            for ($i = 0; $i < 40000; $i++) $bad += stash_get("w{$w}_$i") !== $i;
            stash_put("done$w", 1);
        } else {
            $done = fn() => stash_get("done0") && stash_get("done1") && stash_get("done2") && stash_get("done3");
            for ($n = 0; !$done() && $n < 5000000; $n++) { // until every writer is through: overlaps each grow
                $i = $n % 200;
                $bad += stash_get("f$i") !== ($i % 2 ? $i : ["n" => $i, "s" => "v$i"]);
            }
        }
        exit($bad ? 1 : 0);
    }
    $pids[] = $pid;
}
$failed = 0;
foreach ($pids as $p) { pcntl_waitpid($p, $st); $failed += !pcntl_wifexited($st) || pcntl_wexitstatus($st) !== 0; }
$s = stash_stats();
var_dump($failed, $s['entries'], $s['slots'] > $before);
?>
--EXPECT--
int(0)
int(160204)
bool(true)
