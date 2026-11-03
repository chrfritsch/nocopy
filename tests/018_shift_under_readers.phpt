--TEST--
stash: readers never miss keys that a concurrent delete shifts back in the slot table
--EXTENSIONS--
stash
pcntl
--INI--
apc.shm_size=4M
--FILE--
<?php
// fillers first, then the stable keys: they land behind the fillers' probe runs (16k slots, ~60% full)
for ($i = 0; $i < 6000; $i++) stash_put("fill$i", $i);
for ($i = 0; $i < 3500; $i++) stash_put("s$i", $i % 2 ? $i : "v$i");
$pids = [];
for ($w = 0; $w < 8; $w++) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        $bad = 0;
        if ($w < 4) { // deleting fillers moves the stable keys behind them back, one shift at a time
            for ($i = $w; $i < 6000; $i += 4) { stash_delete("fill$i"); for ($j = 0; $j < 20; $j++) stash_put("tmp$w", $j); }
            stash_put("done$w", 1);
        } else {
            $done = fn() => stash_get("done0") && stash_get("done1") && stash_get("done2") && stash_get("done3");
            while (!$done()) for ($i = 0; $i < 3500; $i++) $bad += stash_get("s$i") !== ($i % 2 ? $i : "v$i");
        }
        exit($bad ? 1 : 0);
    }
    $pids[] = $pid;
}
$failed = 0;
foreach ($pids as $p) { pcntl_waitpid($p, $st); $failed += !pcntl_wifexited($st) || pcntl_wexitstatus($st) !== 0; }
$s = stash_stats();
var_dump($failed, $s['entries'], $s['expunges']);
?>
--EXPECT--
int(0)
int(3508)
int(0)
