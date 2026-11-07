<?php
// Per-op latency percentiles of get and put from forked workers, under eviction and writer contention.
// usage: php [-d ...] tail.php <scenario> [workers=8] [secs=10]      (TAIL_ZERO=1 + NOCOPY_SO: one php -S request, zero-copy)
//   fit: Zipf reads over keys filling half the memory, 1% of hits rewritten (no eviction)
//   evict: Zipf read-through over 1.5x the memory, 20% of hits rewritten (eviction and compaction keep running)
//   contend: half the workers read 64 hot 100-row arrays, half rewrite them
// emits JSON lines like run.php: p50/p99/p99.9/p99.99/max per op, ops/s, and the store's eviction counter.

if (getenv('TAIL_ZERO') && PHP_SAPI !== 'cli-server') {
    putenv('TAIL_ZERO');
    require __DIR__ . '/../tests/serve.inc';
    echo serve('<?php $argv = ' . var_export($argv, true) . '; require ' . var_export(__FILE__, true) . ';', ['extension' => 'pcntl']);
    exit;
}

function out(string $name, float $v, string $unit): void {
    echo json_encode(['name' => $name, 'v' => round($v, 3), 'unit' => $unit]), "\n";
}

function rows(int $n): array {
    $a = [];
    for ($i = 0; $i < $n; $i++) $a["key$i"] = ['id' => $i, 'name' => "name $i", 'tags' => ['a', 'b'], 'score' => $i * 1.5];
    return $a;
}

[, $sc, $w, $secs] = $argv + [1 => 'evict', 8, 10];
$w = (int) $w; $secs = (float) $secs;
$nc = extension_loaded('nocopy');
$mem = (int) ini_get($nc ? 'nocopy.shm_size' : 'apc.shm_size') * 1048576;
$get = $nc ? 'nocopy_get' : 'apcu_fetch';
$str = str_repeat('x', 500);
$arr = rows(10);
$val = fn(int $i) => $i % 4 ? $str : $arr; // 3 in 4 a 500 B string, 1 in 4 a 10-row array
$put = $nc ? fn($k, $v) => nocopy_put($k, $v, 0) : fn($k, $v) => apcu_store($k, $v);

$keys = $sc === 'contend' ? 64 : (int) ($mem * ($sc === 'fit' ? 0.5 : 1.5) / 1600); // nocopy: 600 B per string, 4.5 KB per array, ~1.6 KB on average
$rw = $sc === 'fit' ? 0.01 : 0.2;
if ($sc === 'contend') { $hot = rows(100); for ($i = 0; $i < $keys; $i++) $put("h$i", $hot); }
else for ($i = 0; $i < min($keys, (int) ($mem / 1600)); $i++) $put("r$i", $val($i));
$cdf = []; $s = 0.0;
for ($i = 1; $i <= $keys; $i++) { $s += 1 / $i ** 0.99; $cdf[] = $s; }
$zipf = function () use ($cdf, $s, $keys) {
    $r = mt_rand() / mt_getrandmax() * $s;
    for ($lo = 0, $hi = $keys - 1; $lo < $hi;) { $m = ($lo + $hi) >> 1; if ($cdf[$m] < $r) $lo = $m + 1; else $hi = $m; }
    return $lo;
};

// ponytail: 16 log buckets per octave (~4% resolution), enough for percentiles; max is exact
$dir = sys_get_temp_dir() . '/tail-' . getmypid();
@mkdir($dir);
$ev0 = $nc ? nocopy_stats()['evicted'] : apcu_cache_info(true)['expunges'];
$start = microtime(true) + 0.2;
for ($c = 0; $c < $w; $c++) {
    if (pcntl_fork() === 0) {
        mt_srand($c + 1);
        $h = ['get' => [], 'put' => []]; $max = ['get' => 0, 'put' => 0];
        $writer = $sc === 'contend' && $c % 2;
        while (microtime(true) < $start) usleep(100);
        $end = hrtime(true) + (int) ($secs * 1e9);
        do {
            if ($sc === 'contend') {
                $k = 'h' . mt_rand(0, 63);
                $op = $writer ? 'put' : 'get';
                $t = hrtime(true);
                if ($writer) $put($k, $hot); else $x = $get($k);
                $d = hrtime(true) - $t;
            } else {
                $i = $zipf(); $k = "r$i";
                $t = hrtime(true); $x = $get($k, $hit); $d = hrtime(true) - $t;
                $op = 'get';
                $b = (int) (log(max($d, 1), 2) * 16); $h[$op][$b] = ($h[$op][$b] ?? 0) + 1; if ($d > $max[$op]) $max[$op] = $d;
                if ($hit && mt_rand() / mt_getrandmax() >= $rw) continue;
                $op = 'put';
                $t = hrtime(true); $put($k, $val($i)); $d = hrtime(true) - $t;
            }
            $b = (int) (log(max($d, 1), 2) * 16); $h[$op][$b] = ($h[$op][$b] ?? 0) + 1; if ($d > $max[$op]) $max[$op] = $d;
        } while ($t < $end);
        file_put_contents("$dir/$c", serialize([$h, $max]));
        if (PHP_SAPI === 'cli-server') pcntl_exec('/bin/true');
        exit(0);
    }
}
while (pcntl_wait($st) > 0);
$H = ['get' => [], 'put' => []]; $M = ['get' => 0, 'put' => 0];
foreach (glob("$dir/*") as $f) {
    [$h, $max] = unserialize(file_get_contents($f)); unlink($f);
    foreach ($h as $op => $bs) foreach ($bs as $b => $n) $H[$op][$b] = ($H[$op][$b] ?? 0) + $n;
    foreach ($max as $op => $m) $M[$op] = max($M[$op], $m);
}
rmdir($dir);
$p = "$sc, $w workers:";
foreach ($H as $op => $bs) {
    if (!$bs) continue;
    ksort($bs);
    $n = array_sum($bs);
    out("$p $op ops", $n / $secs, 'ops/s');
    foreach ([50, 99, 99.9, 99.99] as $q) {
        $need = $n * $q / 100; $acc = 0;
        foreach ($bs as $b => $c) if (($acc += $c) >= $need) break;
        out("$p $op p$q", 2 ** (($b + 1) / 16) / 1000, 'us');
    }
    out("$p $op max", $M[$op] / 1000, 'us');
}
out("$p " . ($nc ? 'evicted' : 'expunges'), ($nc ? nocopy_stats()['evicted'] : apcu_cache_info(true)['expunges']) - $ev0, 'count');
