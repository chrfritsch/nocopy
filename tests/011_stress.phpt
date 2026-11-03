--TEST--
stash: random puts/deletes of mixed sizes agree with a PHP array model; held values stay intact
--EXTENSIONS--
stash
--INI--
apc.shm_size=4M
stash.request_scope=1
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
            if (stash_put($k, $v)) { $model[$k] = $v; } else { $full++; unset($model[$k]); stash_delete($k); }
            break;
        case 4: case 5:
            stash_delete($k); unset($model[$k]);
            break;
        case 6:
            if (isset($model[$k])) { $held[$k] = [stash_get($k), $model[$k]]; } // zero-copy value held across later churn
            break;
        case 7:
            if ($held && mt_rand(0, 3) === 0) { $held = []; stash_checkpoint(); }
            break;
        default:
            $got = stash_get($k, $hit);
            if ($hit !== isset($model[$k]) || ($hit && $got !== $model[$k])) $bad++;
    }
}
foreach ($held as [$a, $b]) if ($a !== $b) $bad++;       // never overwritten while pinned
foreach ($model as $k => $v) if (stash_get($k) !== $v) $bad++;
$s = stash_stats();
var_dump($bad, $s['entries'] === count($model), $s['used'] <= $s['size']);
?>
--EXPECT--
int(0)
bool(true)
bool(true)
