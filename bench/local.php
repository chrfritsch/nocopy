<?php
// Copy mode kept copies (nocopy.local_size): ns per fetch of N arrays under a key distribution, the CLI being copy mode.
// usage: php -d nocopy.local_size=.. local.php <zipf|loop> [keys=3000] [rows=20]
[, $wl, $n, $rows] = $argv + [1 => 'zipf', 3000, 20];
$n = (int) $n; $rows = (int) $rows;
for ($i = 0; $i < $n; $i++) {
    $a = [];
    for ($r = 0; $r < $rows; $r++) $a[] = ['id' => $r, 'name' => "row $r of $i", 'score' => $r * 1.5];
    nocopy_put("a$i", $a);
}
mt_srand(42);
$seq = [];
if ($wl === 'zipf') {
    $cdf = []; $s = 0.0;
    for ($i = 1; $i <= $n; $i++) { $s += 1 / $i ** 0.99; $cdf[] = $s; }
    for ($q = 0; $q < 300000; $q++) {
        $x = mt_rand() / mt_getrandmax() * $s;
        for ($lo = 0, $hi = $n - 1; $lo < $hi;) { $m = ($lo + $hi) >> 1; if ($cdf[$m] < $x) $lo = $m + 1; else $hi = $m; }
        $seq[] = "a$lo";
    }
} else {
    for ($q = 0; $q < 300000; $q++) $seq[] = 'a' . ($q % $n);
}
foreach ($seq as $k) nocopy_get($k); // warm
$best = INF;
for ($r = 0; $r < 3; $r++) {
    $t = hrtime(true);
    foreach ($seq as $k) nocopy_get($k);
    $best = min($best, (hrtime(true) - $t) / count($seq));
}
echo json_encode(['name' => "$wl keys=$n rows=$rows local_size=" . ini_get('nocopy.local_size'), 'v' => round($best, 1), 'unit' => 'ns/fetch']), "\n";
