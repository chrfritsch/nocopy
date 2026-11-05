--TEST--
nocopy: random puts/deletes of mixed sizes agree with a PHP array model; held values stay intact
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t011
nocopy.shm_size=4M
nocopy.request_scope=1
--FILE--
<?php
mt_srand(42);
$model = [];
$held = [];
$bad = 0; $full = 0;
$gen = function ($i) {
    $n = [1, 5, 50, 500, 3000, 6000][mt_rand(0, 5)]; // up to ~100 KB: spans normal entries and multi-segment runs
    return $n === 1 ? "s$i" : array_fill(0, $n, "v$i");
};
for ($i = 0; $i < 6000; $i++) {
    $k = 'k' . mt_rand(0, 120);
    switch (mt_rand(0, 9)) {
        case 0: case 1: case 2: case 3:
            $v = $gen($i);
            $e = nocopy_stats()['expunges'];
            if (nocopy_put($k, $v)) { $model[$k] = $v; $at[$k] = $e; } else { $full++; unset($model[$k]); nocopy_delete($k); }
            break;
        case 4: case 5:
            nocopy_delete($k); unset($model[$k]);
            break;
        case 6:
            if (isset($model[$k])) { $held[$k] = [nocopy_get($k), $model[$k]]; } // zero-copy value held across later churn
            break;
        case 7:
            if ($held && mt_rand(0, 3) === 0) { $held = []; } // pins stay until the request ends
            break;
        default:
            $got = nocopy_get($k, $hit);
            if ($hit ? !isset($model[$k]) || $got !== $model[$k] : isset($model[$k]) && nocopy_stats()['expunges'] === $at[$k]) $bad++; // a miss must be an eviction
    }
}
foreach ($held as [$a, $b]) if ($a !== $b) $bad++;       // never overwritten while pinned
foreach ($model as $k => $v) if (($g = nocopy_get($k)) !== $v && ($g !== null || nocopy_stats()['expunges'] === $at[$k])) $bad++;
$s = nocopy_stats();
var_dump($bad, $s['entries'] <= count($model), $s['used'] <= $s['size']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t011*')); ?>
--EXPECT--
int(0)
bool(true)
bool(true)
