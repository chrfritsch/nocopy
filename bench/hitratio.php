<?php
// Hit ratio of a read-through cache (get; on miss put) under one workload, sized relative to the arena.
// usage: php -d apc.shm_size=.. hitratio.php <workload> [footprint=1.5] [ohw=0] [w=0]
//   workload: zipf (alpha 0.99 over the keys), loop (keys in order, cyclically)
//   footprint: distinct recurring keys' bytes / arena bytes; ohw: one-hit-wonder puts per request; w: share of hits rewritten (overwrites)
// prints one JSON line like run.php.

[, $wl, $fp, $ohw, $w] = $argv + [1 => 'zipf', 1.5, 0, 0];
$fp = (float) $fp; $ohw = (float) $ohw; $w = (float) $w;
$v = str_repeat('x', 500);
$per = 640; // bytes an entry takes in the arena (header + zval + key + 500 B)
$keys = (int) (nocopy_stats()['size'] * $fp / $per);
$reqs = 40 * $keys;
mt_srand(42);
if ($wl === 'zipf') {
    $cdf = []; $s = 0.0;
    for ($i = 1; $i <= $keys; $i++) { $s += 1 / $i ** 0.99; $cdf[] = $s; }
    $next = function () use ($cdf, $s, $keys) {
        $r = mt_rand() / mt_getrandmax() * $s;
        for ($lo = 0, $hi = $keys - 1; $lo < $hi;) { $m = ($lo + $hi) >> 1; if ($cdf[$m] < $r) $lo = $m + 1; else $hi = $m; }
        return $lo;
    };
} else {
    $i = 0;
    $next = function () use (&$i, $keys) { return $i++ % $keys; };
}
$hits = $n = $u = 0; $acc = 0.0; $t0 = hrtime(true);
for ($q = 0; $q < $reqs; $q++) {
    for ($acc += $ohw; $acc >= 1; $acc--) nocopy_put('u' . $u++, $v);
    $k = 'r' . $next();
    nocopy_get($k, $hit);
    if (!$hit || ($w && mt_rand() / mt_getrandmax() < $w)) nocopy_put($k, $v);
    if ($q >= $reqs / 4) { $n++; $hits += $hit; } // first quarter warms up
}
$st = nocopy_stats();
echo json_encode(['name' => "$wl fp=$fp ohw=$ohw w=$w", 'v' => round(100 * $hits / $n, 1), 'unit' => '% hits',
    'evicted' => $st['evicted'], 'ghost_hits' => $st['ghost_hits'] ?? null,
    'moved_mb' => round($st['evacuated_bytes'] / 1e6, 1), 'ns' => (int) ((hrtime(true) - $t0) / $reqs)]), "\n";
