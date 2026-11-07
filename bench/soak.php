<?php
// Soak: long mixed workload from forked CLI workers (copy mode, long-lived), one line per interval to watch for drift.
// usage: php [-d nocopy.*] soak.php [minutes=60] [workers=8] [interval_s=30]
//   per op: 88% read-through get (Zipf over a key set 2x the arena that shifts every 5 minutes), 8% put, 2% delete, 2% inc;
//   values 100 B-20 KB strings and arrays, 1 in 2000 (cold keys) a 0.5-2 MB string; TTL none / 30 s / 300 s by key.
//   SOAK_FP=x: key set x times the arena (default 2); SOAK_RO_AT=m: from minute m on, reads only (no put, delete or inc)
// emits one JSON line per interval: hit %, get/put percentiles (us), ops/s, arena stats, max private RSS of a worker (MB).

function rows(int $n): array {
    $a = [];
    for ($i = 0; $i < $n; $i++) $a["key$i"] = ['id' => $i, 'name' => "name $i", 'tags' => ['a', 'b'], 'score' => $i * 1.5];
    return $a;
}
function pct(array $h, float $q): float {
    if (!$h) return 0;
    ksort($h); $need = array_sum($h) * $q / 100; $acc = 0;
    foreach ($h as $b => $c) if (($acc += $c) >= $need) break;
    return round(2 ** (($b + 1) / 16) / 1000, 1);
}

[, $mins, $w, $iv] = $argv + [1 => 60, 8, 30];
$w = (int) $w; $iv = (int) $iv; $ticks = (int) ($mins * 60 / $iv);
$keys = (int) (nocopy_stats()["size"] * (getenv('SOAK_FP') ?: 2) / 7000); // ~7 KB average value
$cdf = []; $s = 0.0;
for ($i = 1; $i <= $keys; $i++) { $s += 1 / $i ** 0.99; $cdf[] = $s; }
$vals = [];
foreach ([100, 1000, 5000, 20000] as $n) $vals[] = str_repeat('x', $n);
foreach ([1, 10, 40] as $n) $vals[] = rows($n);
$dir = sys_get_temp_dir() . '/soak-' . getmypid();
@mkdir($dir);
$t0 = microtime(true) + 0.5;
$pids = [];
for ($c = 0; $c < $w; $c++) {
    if (($pid = pcntl_fork()) === 0) {
        mt_srand($c + 1);
        for ($tick = 0; $tick < $ticks; $tick++) {
            $h = ['get' => [], 'put' => []]; $mx = 0; $hits = $n = 0;
            $end = $t0 + ($tick + 1) * $iv;
            $epoch = (int) ($tick * $iv / 300);
            $ro = ($ra = getenv('SOAK_RO_AT')) !== false && $tick * $iv >= $ra * 60;
            while (microtime(true) < $end) {
                $r = mt_rand() / mt_getrandmax() * $s;
                for ($lo = 0, $hi = $keys - 1; $lo < $hi;) { $m = ($lo + $hi) >> 1; if ($cdf[$m] < $r) $lo = $m + 1; else $hi = $m; }
                $k = 'e' . ($epoch + ($lo % 3 === 0 ? 0 : 1)) . ":$lo"; // a third of the keys survive an epoch change
                $ttl = [0, 30, 300][$lo % 3];
                $v = $lo % 2000 === 1999 ? str_repeat('b', 524288 << ($lo % 3)) : $vals[$lo % 7];
                $op = mt_rand(1, 100);
                if ($op <= 88) {
                    $t = hrtime(true); nocopy_get($k, $hit); $d = hrtime(true) - $t;
                    $b = (int) (log(max($d, 1), 2) * 16); $h['get'][$b] = ($h['get'][$b] ?? 0) + 1;
                    $n++; $hits += $hit;
                    if ($hit || $ro) continue;
                } elseif ($ro) { continue;
                } elseif ($op <= 96) {
                } elseif ($op <= 98) { nocopy_delete($k); continue; }
                else { nocopy_inc("cnt" . ($lo % 64), 1); continue; }
                $t = hrtime(true); nocopy_put($k, $v, $ttl); $d = hrtime(true) - $t;
                $b = (int) (log(max($d, 1), 2) * 16); $h['put'][$b] = ($h['put'][$b] ?? 0) + 1; if ($d > $mx) $mx = $d;
            }
            preg_match('/RssAnon:\s+(\d+)/', file_get_contents('/proc/self/status'), $rss);
            file_put_contents("$dir/$tick-$c", serialize([$h, $mx, $hits, $n, (int) $rss[1]]));
        }
        exit(0);
    }
    $pids[] = $pid;
}
$last = nocopy_stats();
for ($tick = 0; $tick < $ticks; $tick++) {
    time_sleep_until($t0 + ($tick + 1) * $iv + 1);
    $H = ['get' => [], 'put' => []]; $mx = $hits = $n = $rss = 0;
    foreach (glob("$dir/$tick-*") as $f) {
        [$h, $m, $hi, $nn, $r] = unserialize(file_get_contents($f)); unlink($f);
        foreach ($h as $op => $bs) foreach ($bs as $b => $c) $H[$op][$b] = ($H[$op][$b] ?? 0) + $c;
        $mx = max($mx, $m); $hits += $hi; $n += $nn; $rss = max($rss, $r);
    }
    $st = nocopy_stats();
    echo json_encode(['min' => round(($tick + 1) * $iv / 60, 1), 'hit%' => $n ? round(100 * $hits / $n, 1) : 0,
        'gets/s' => (int) ($n / $iv), 'get p99.9' => pct($H['get'], 99.9), 'put p99' => pct($H['put'], 99), 'put p99.99' => pct($H['put'], 99.99),
        'put max' => round($mx / 1000), 'used MB' => round($st['used'] / 1048576, 1), 'entries' => $st['entries'],
        'free/dead/pinned segs' => "$st[free_segments]/$st[dead_segments]/$st[pinned_segments]",
        'evicted' => $st['evicted'] - $last['evicted'], 'expired' => $st['expired'] - $last['expired'],
        'moved MB' => round(($st['evacuated_bytes'] - $last['evacuated_bytes']) / 1048576), 'steals' => $st['lock_steals'],
        'worker RssAnon MB' => round($rss / 1024, 1)]), "\n";
    $last = $st;
}
while (pcntl_wait($x) > 0);
@rmdir($dir);
