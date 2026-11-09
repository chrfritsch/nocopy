<?php
// Read-through replay of a prepared trace (docker/trace.sh): get; on a miss put the value with the request's TTL.
// usage: php -d nocopy.shm_size=.. trace.php <time,key,value bytes,ttl,object bytes csv> [requests=all]
// The clock is the trace's: run under libfaketime with FAKETIME_NO_CACHE=1 (frozen, set per trace second).
// prints one JSON line like hitratio.php; ratios count requests and object bytes, as libCacheSim's cachesim does.

[, $file, $max] = $argv + [2 => PHP_INT_MAX];
$f = fopen($file, 'r') or exit(1);
$t0 = 1583020800; // 2020-03-01, the traces' week
$sec = -1; $n = $hits = $bytes = $hitBytes = 0; $max = (int) $max; $wall = hrtime(true);
while ($n < $max && ($l = fgets($f)) !== false) {
    [$ts, $k, $vb, $ttl, $ob] = explode(',', rtrim($l));
    if ($ts != $sec) putenv('FAKETIME=' . gmdate('Y-m-d H:i:s', $t0 + ($sec = (int) $ts)));
    $n++; $bytes += $ob;
    nocopy_get($k, $hit);
    if ($hit) { $hits++; $hitBytes += $ob; }
    else nocopy_put($k, str_repeat('x', (int) $vb), (int) $ttl);
}
$s = nocopy_stats();
echo json_encode(['name' => basename($file) . ' nocopy ' . ini_get('nocopy.shm_size'), 'v' => round(100 * $hits / $n, 2), 'unit' => '% hits',
    'byte_hits' => round(100 * $hitBytes / $bytes, 2), 'requests' => $n, 'trace_s' => $sec, 'evicted' => $s['evicted'],
    'moved_mb' => round($s['evacuated_bytes'] / 1e6, 1), 'ns' => (int) ((hrtime(true) - $wall) / $n)]), "\n";
