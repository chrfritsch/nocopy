<?php
// Cache stampede: $w forked workers serve "requests" that each read one hot key with a short TTL and a slow callback.
// usage: php [-d ...] stampede.php <mode> [workers=8] [secs=10] [ttl=2] [compute_ms=50] [think_ms=1]
//   mode: naive (fetch, on miss compute + store), entry (apcu_entry), remember (nocopy_remember, XFetch)
// emits JSON lines like run.php: callback runs per expiry, requests/s, slow requests, p99.9 and max latency.

function out(string $name, float $v, string $unit): void {
    echo json_encode(['name' => $name, 'v' => round($v, 3), 'unit' => $unit]), "\n";
}

if (extension_loaded('nocopy')) require __DIR__ . '/../apcu/apcu.php'; // naive and entry go through apcu_*

[, $mode, $w, $secs, $ttl, $ms, $think] = $argv + [1 => 'naive', 8, 10, 2, 50, 1];
$w = (int) $w; $secs = (float) $secs; $ttl = (int) $ttl; $ms = (int) $ms; $think = (int) $think;
$runs = tempnam(sys_get_temp_dir(), 'stampede');
$compute = function () use ($runs, $ms) {
    file_put_contents($runs, '.', FILE_APPEND); // one byte per callback run: atomic append, no cache involved
    usleep($ms * 1000);
    return str_repeat('v', 1000);
};
$get = match ($mode) {
    'naive' => function () use ($compute, $ttl) {
        $v = apcu_fetch('hot', $ok);
        if (!$ok) { $v = $compute(); apcu_store('hot', $v, $ttl); }
        return $v;
    },
    'entry' => fn() => apcu_entry('hot', $compute, $ttl),
    'remember' => fn() => nocopy_remember('hot', $compute, $ttl),
};
apcu_delete('hot');
$dir = sys_get_temp_dir() . '/stampede-' . getmypid();
@mkdir($dir);
$start = microtime(true) + 0.2; // all workers begin together
for ($i = 0; $i < $w; $i++) {
    if (pcntl_fork() === 0) {
        while (microtime(true) < $start) usleep(100);
        $end = $start + $secs;
        $lat = [];
        while (($t = hrtime(true)) && microtime(true) < $end) {
            $get();
            $lat[] = (hrtime(true) - $t) / 1e6;
            if ($think > 1) { $t = microtime(true) * 1000; usleep((int) (($think - fmod($t, $think)) * 1000) + mt_rand(0, 5000)); } // bursts: all wake on the same tick after idle
            else usleep(1000); // the rest of the request
        }
        file_put_contents("$dir/$i", serialize($lat));
        exit(0);
    }
}
while (pcntl_wait($st) > 0);
$lat = [];
foreach (glob("$dir/*") as $f) { array_push($lat, ...unserialize(file_get_contents($f))); unlink($f); }
rmdir($dir);
sort($lat);
$n = count($lat);
$calls = filesize($runs);
unlink($runs);
$prefix = "$mode, $w workers, ttl {$ttl}s, compute {$ms}ms, think {$think}ms:";
out("$prefix callback runs per expiry", $calls / ($secs / $ttl), 'runs');
out("$prefix requests", $n / $secs, 'req/s');
out("$prefix requests slower than " . ($ms / 2) . "ms", count(array_filter($lat, fn($l) => $l > $ms / 2)), 'req');
out("$prefix p99.9", $lat[(int) ($n * 0.999)], 'ms');
out("$prefix max", $lat[$n - 1], 'ms');
