--TEST--
stash: readers never miss keys that a concurrent delete shifts back in the slot table
--EXTENSIONS--
stash
pcntl
--INI--
apc.shm_size=4M
--FILE--
<?php
// Home slot of a key in this 16k-slot table: mirrors stash_slot_tag() (bits below 2^46 suffice).
function home(string $k): int {
    $m = (1 << 46) - 1; $h = 5381;
    for ($i = 0, $n = strlen($k); $i < $n; $i++) $h = ($h * 33 + ord($k[$i])) & $m;
    $k0 = 0x2E3D27D4EB4F & 0x7FFFFF; $k1 = 0x2E3D27D4EB4F >> 23;
    $a0 = $h & 0x7FFFFF; $a1 = $h >> 23;
    return ((($a0 * $k0 + ((($a0 * $k1 + $a1 * $k0) & 0x7FFFFF) << 23)) & $m) >> 32) & 16383;
}
// keys homed in the first half only: one long probe run, so shifts move keys far under the readers
$pick = function (string $p, int $want): array {
    for ($r = [], $n = 0; count($r) < $want; $n++) if (home("$p$n") < 8192) $r[] = "$p$n";
    return $r;
};
$fill = $pick("fill", 6000);
$stable = $pick("s", 3500);
foreach ($fill as $i => $k) stash_put($k, $i);
foreach ($stable as $i => $k) stash_put($k, $i % 2 ? $i : "v$i");
$pids = [];
for ($w = 0; $w < 8; $w++) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        $bad = 0;
        if ($w < 4) { // deleting fillers moves the stable keys behind them back, one shift at a time
            for ($i = $w; $i < 6000; $i += 4) { stash_delete($fill[$i]); for ($j = 0; $j < 20; $j++) stash_put("tmp$w", $j); }
            stash_put("done$w", 1);
        } else {
            $done = fn() => stash_get("done0") && stash_get("done1") && stash_get("done2") && stash_get("done3");
            while (!$done()) foreach ($stable as $i => $k) $bad += stash_get($k) !== ($i % 2 ? $i : "v$i");
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
