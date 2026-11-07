<?php
// Large arenas: fill until eviction starts, then 4 readers + 4 writers (new keys, so eviction never stops) for <secs>.
// usage: php -d nocopy.shm_size=8G big.php [secs=20]
// emits JSON lines like run.php: fill rate, arena layout, get/put percentiles and max during eviction, merge counters.

function out(string $name, float $v, string $unit): void {
    echo json_encode(['name' => $name, 'v' => round($v, 3), 'unit' => $unit]), "\n";
}
function rows(int $n): array {
    $a = [];
    for ($i = 0; $i < $n; $i++) $a["key$i"] = ['id' => $i, 'name' => "name $i", 'tags' => ['a', 'b'], 'score' => $i * 1.5];
    return $a;
}

$secs = (float) ($argv[1] ?? 20);
$str = str_repeat('x', 500); $arr = rows(10);
$st = nocopy_stats();
$gb = $st['size'] >= 2 ** 30 ? round($st['size'] / 2 ** 30) . 'G' : round($st['size'] / 2 ** 20) . 'M';
out("$gb segment size", $st['segment_size'] / 1048576, 'MB');
out("$gb shards", $st['shards'], 'count');
$t = hrtime(true); $n = 0;
while (nocopy_stats()['evicted'] === 0 || $n % 10000) { nocopy_put("k$n", $n % 4 ? $str : $arr, 0); $n++; }
$e = (hrtime(true) - $t) / 1e9;
out("$gb fill until eviction", $e, 's');
out("$gb entries when full", $n, 'count');
out("$gb fill rate", $n / $e, 'puts/s');
$st0 = nocopy_stats();

$dir = sys_get_temp_dir() . '/big-' . getmypid();
@mkdir($dir);
$start = microtime(true) + 0.2;
for ($c = 0; $c < 8; $c++) {
    if (pcntl_fork() === 0) {
        mt_srand($c + 1);
        $op = $c % 2 ? 'put' : 'get'; $h = []; $max = 0; $k = $n + $c;
        while (microtime(true) < $start) usleep(100);
        $end = hrtime(true) + (int) ($secs * 1e9);
        do {
            if ($op === 'get') { $i = (int) ($n * (mt_rand() / mt_getrandmax()) ** 3); $t = hrtime(true); $x = nocopy_get("k$i"); }
            else { $t = hrtime(true); nocopy_put('k' . ($k += 8), $k % 4 ? $str : $arr, 0); }
            $d = hrtime(true) - $t;
            $b = (int) (log(max($d, 1), 2) * 16); $h[$b] = ($h[$b] ?? 0) + 1; if ($d > $max) $max = $d;
        } while ($t < $end);
        file_put_contents("$dir/$c", serialize([$op, $h, $max]));
        exit(0);
    }
}
while (pcntl_wait($x) > 0);
$H = ['get' => [], 'put' => []]; $M = ['get' => 0, 'put' => 0];
foreach (glob("$dir/*") as $f) {
    [$op, $h, $max] = unserialize(file_get_contents($f)); unlink($f);
    foreach ($h as $b => $c) $H[$op][$b] = ($H[$op][$b] ?? 0) + $c;
    $M[$op] = max($M[$op], $max);
}
rmdir($dir);
foreach ($H as $op => $bs) {
    ksort($bs); $all = array_sum($bs);
    out("$gb $op ops", $all / $secs, 'ops/s');
    foreach ([50, 99, 99.9, 99.99] as $q) {
        $need = $all * $q / 100; $acc = 0;
        foreach ($bs as $b => $c) if (($acc += $c) >= $need) break;
        out("$gb $op p$q", 2 ** (($b + 1) / 16) / 1000, 'us');
    }
    out("$gb $op max", $M[$op] / 1000, 'us');
    foreach ([1, 10, 50] as $ms) out("$gb {$op}s over $ms ms", array_sum(array_filter($bs, fn($b) => 2 ** ($b / 16) > $ms * 1e6, ARRAY_FILTER_USE_KEY)), 'count');
}
$st = nocopy_stats();
out("$gb evicted per s", ($st['evicted'] - $st0['evicted']) / $secs, 'entries/s');
out("$gb eviction rounds per s", ($st['expunges'] - $st0['expunges']) / $secs, 'rounds/s');
out("$gb moved per round", ($st['evacuated_bytes'] - $st0['evacuated_bytes']) / max(1, $st['expunges'] - $st0['expunges']) / 1048576, 'MB');
