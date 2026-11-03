--TEST--
stash: 8 processes writing, deleting and reading their own keys under memory pressure all agree with their models
--EXTENSIONS--
stash
pcntl
--INI--
apc.shm_size=16M
--FILE--
<?php
$pids = [];
for ($w = 0; $w < 8; $w++) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        mt_srand($w);
        $model = []; $bad = 0;
        for ($i = 0; $i < 20000; $i++) {
            $k = "w{$w}_" . mt_rand(0, 150);
            $r = mt_rand(0, 9);
            if ($r < 4) {
                $n = [1, 5, 50, 500, 2000][mt_rand(0, 4)];
                $v = $n === 1 ? "s$i" : array_fill(0, $n, "v$w.$i");
                $e = stash_stats()['expunges'];
                if (stash_put($k, $v, mt_rand(0, 3) ? 0 : 100)) { $model[$k] = $v; $at[$k] = $e; } else { stash_delete($k); unset($model[$k]); }
            } elseif ($r < 6) {
                stash_delete($k); unset($model[$k]);
            } else {
                $got = stash_get($k, $hit);
                if ($hit ? !isset($model[$k]) || $got !== $model[$k] : isset($model[$k]) && stash_stats()['expunges'] === $at[$k]) $bad++; // a miss must be an eviction
            }
        }
        foreach ($model as $k => $v) if (($g = stash_get($k)) !== $v && ($g !== null || stash_stats()['expunges'] === $at[$k])) $bad++;
        exit($bad ? 1 : 0);
    }
    $pids[] = $pid;
}
$failed = 0;
foreach ($pids as $p) { pcntl_waitpid($p, $st); $failed += !pcntl_wifexited($st) || pcntl_wexitstatus($st) !== 0; }
$s = stash_stats();
var_dump($failed, $s['used'] <= $s['size']);
?>
--EXPECT--
int(0)
bool(true)
