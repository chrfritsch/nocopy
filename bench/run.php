<?php
// usage: php [-d ...] run.php <group>   groups: read, write, parallel, mixed, contend, ttl
// emits one JSON line per measurement: {"name":..., "v":..., "unit":...}

function out(string $name, float $v, string $unit): void {
    echo json_encode(['name' => $name, 'v' => round($v, 3), 'unit' => $unit]), "\n";
}

/** Median ns/op of $fn($n) over 5 reps; n auto-scaled so a rep takes ~120ms. */
function bench(string $name, callable $fn): void {
    $n = 100;
    do { $n *= 2; $t = hrtime(true); $fn($n); $e = hrtime(true) - $t; } while ($e < 20e6);
    $n = (int) ($n * 120e6 / $e) + 1;
    $fn((int) ($n / 10) + 1); // warm
    $s = [];
    for ($r = 0; $r < 5; $r++) { $t = hrtime(true); $fn($n); $s[] = (hrtime(true) - $t) / $n; }
    sort($s);
    out($name, $s[2], 'ns/op');
}

function rows(int $n): array {
    $a = [];
    for ($i = 0; $i < $n; $i++) $a["key$i"] = ['id' => $i, 'name' => "name $i", 'tags' => ['a', 'b'], 'score' => $i * 1.5];
    return $a;
}

class Obj { public $a = 1; public $b = 'two'; public $c = [1, 2, 3]; public ?Obj $child = null; }

$group = $argv[1] ?? 'read';
if (!apcu_enabled() || ini_get('apc.shm_size') !== '512M') {
    fwrite(STDERR, "bench misconfigured: enabled=" . var_export(apcu_enabled(), true) . " shm=" . ini_get('apc.shm_size') . "\n");
    exit(1);
}

if ($group === 'read') {
    apcu_store('int', 12345);
    apcu_store('str100', str_repeat('x', 100));
    apcu_store('str10k', str_repeat('y', 10000));
    apcu_store('arr10', array_slice(rows(10), 0, 10));
    apcu_store('arr1k', rows(1000));
    apcu_store('arr10k', rows(10000));
    apcu_store('arr1k_u', unserialize(serialize(rows(1000)))); // every key its own string, as from unserialize/json_decode
    if (function_exists('nocopy_stats')) {
        $u = nocopy_stats()['used']; apcu_store('mem1k', rows(1000)); out('arena bytes, array 1k rows', nocopy_stats()['used'] - $u, 'bytes');
    }
    $o = new Obj; $o->child = new Obj; apcu_store('obj', $o);

    bench('fetch int', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('int'); });
    bench('fetch string 100B', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('str100'); });
    bench('fetch string 10KB', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('str10k'); });
    bench('fetch array 10 rows', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('arr10'); });
    bench('fetch array 1k rows', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('arr1k'); });
    bench('fetch array 1k rows, unserialized keys', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('arr1k_u'); });
    bench('fetch array 10k rows', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('arr10k'); });
    bench('fetch 10k rows + read 1', function ($n) { for ($i = 0; $i < $n; $i++) { $x = apcu_fetch('arr10k'); $y = $x['key9999']['name']; } });
    bench('fetch 10k rows + sum ALL scores', function ($n) { for ($i = 0; $i < $n; $i++) { $t = 0.0; foreach (apcu_fetch('arr10k') as $row) $t += $row['score']; } });
    bench('fetch 10k rows + modify 1 (COW)', function ($n) { for ($i = 0; $i < $n; $i++) { $x = apcu_fetch('arr10k'); $x['key5']['id'] = -1; } });
    bench('fetch object', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('obj'); });
    bench('fetch miss', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch('nokey'); });
    bench('exists', function ($n) { for ($i = 0; $i < $n; $i++) $x = apcu_exists('int'); });
    $keys = []; for ($i = 0; $i < 100000; $i++) { $keys[] = "d$i"; apcu_store("d$i", $i); }
    bench('fetch int, 100k distinct keys', function ($n) use ($keys) { for ($i = 0; $i < $n; $i++) $x = apcu_fetch($keys[$i % 100000]); });
    bench('apcu_entry hit', function ($n) { $f = fn() => 1; for ($i = 0; $i < $n; $i++) $x = apcu_entry('int', $f); });
    bench('fetch 10 keys (array form)', function ($n) { $k = array_map(fn($i) => "m$i", range(0, 9)); for ($i = 0; $i < 10; $i++) apcu_store($k[$i], $i); for ($i = 0; $i < $n; $i++) $x = apcu_fetch($k); });
}

if ($group === 'write') {
    $s100 = str_repeat('x', 100); $a10 = array_slice(rows(10), 0, 10); $a1k = rows(1000); $a1ku = unserialize(serialize($a1k));
    bench('store int', function ($n) { for ($i = 0; $i < $n; $i++) apcu_store('w_int', $i); });
    bench('store string 100B', function ($n) use ($s100) { for ($i = 0; $i < $n; $i++) apcu_store('w_str', $s100); });
    bench('store array 10 rows', function ($n) use ($a10) { for ($i = 0; $i < $n; $i++) apcu_store('w_a10', $a10); });
    bench('store array 1k rows', function ($n) use ($a1k) { for ($i = 0; $i < $n; $i++) apcu_store('w_a1k', $a1k); });
    bench('store array 1k rows, unserialized keys', function ($n) use ($a1ku) { for ($i = 0; $i < $n; $i++) apcu_store('w_a1ku', $a1ku); });
    $o = new Obj; $o->child = new Obj;
    bench('store object', function ($n) use ($o) { for ($i = 0; $i < $n; $i++) apcu_store('w_obj', $o); });
    bench('store+delete int', function ($n) { for ($i = 0; $i < $n; $i++) { apcu_store('w_k', $i); apcu_delete('w_k'); } });
    apcu_store('ctr', 0);
    bench('inc counter', function ($n) { for ($i = 0; $i < $n; $i++) apcu_inc('ctr'); });
    bench('add (exists, fails)', function ($n) { apcu_store('ad', 1); for ($i = 0; $i < $n; $i++) apcu_add('ad', 2); });
}

if ($group === 'parallel') {
    // N workers each hammering fetches for a fixed op count; report aggregate ops/sec
    apcu_store('arr1k', rows(1000));
    apcu_store('int', 1);
    foreach (['int' => 400000, 'arr1k' => 3000] as $key => $ops) {
        foreach ([1, 2, 4, 8] as $w) {
            $t = hrtime(true);
            $pids = [];
            for ($k = 0; $k < $w; $k++) {
                $pid = pcntl_fork();
                if ($pid === 0) { for ($i = 0; $i < $ops; $i++) $x = apcu_fetch($key); exit(0); }
                $pids[] = $pid;
            }
            foreach ($pids as $p) pcntl_waitpid($p, $st);
            $secs = (hrtime(true) - $t) / 1e9;
            out("parallel fetch $key x$w workers", $w * $ops / $secs / 1e6, 'M ops/s');
        }
    }
}

/** Forks $w children running $fn($end, $k) for $secs; returns aggregate M ops/s ($fn returns its op count). */
function forked(int $w, float $secs, callable $fn): float {
    $tmp = tempnam(sys_get_temp_dir(), 'bench');
    $pids = [];
    for ($k = 0; $k < $w; $k++) {
        $pid = pcntl_fork();
        if ($pid === 0) { $end = hrtime(true) + $secs * 1e9; $n = $fn($end, $k); file_put_contents($tmp, "$n\n", FILE_APPEND | LOCK_EX); exit(0); }
        $pids[] = $pid;
    }
    foreach ($pids as $p) pcntl_waitpid($p, $st);
    $total = array_sum(array_map('intval', file($tmp)));
    unlink($tmp);
    return $total / $secs / 1e6;
}

if ($group === 'contend') {
    // writers fighting for the lock; 16 workers oversubscribe the cores
    foreach ([1, 4, 8, 16] as $w) {
        out("parallel store int x$w workers", forked($w, 1.0, function ($end, $k) {
            for ($n = 0; hrtime(true) < $end; $n += 100) for ($i = 0; $i < 100; $i++) apcu_store("c$k", $i);
            return $n;
        }), 'M ops/s');
    }
    // readers of a scalar while 4 writers store other keys: readers must not share the writers' cache lines
    apcu_store('int', 1);
    foreach ([0, 4] as $writers) {
        apcu_store('stop', 0);
        $pids = [];
        for ($k = 0; $k < $writers; $k++) {
            $pid = pcntl_fork();
            if ($pid === 0) { while (!apcu_fetch('stop')) for ($i = 0; $i < 100; $i++) apcu_store("wr$k", $i); exit(0); }
            $pids[] = $pid;
        }
        out("4 readers fetch int, $writers writers", forked(4, 1.0, function ($end) {
            for ($n = 0; hrtime(true) < $end; $n += 100) for ($i = 0; $i < 100; $i++) $x = apcu_fetch('int');
            return $n;
        }), 'M ops/s');
        apcu_store('stop', 1);
        foreach ($pids as $p) pcntl_waitpid($p, $st);
    }
}

if ($group === 'ttl') {
    // mixed-TTL churn under memory pressure: 50k long-lived values rewritten at random, plus a stream of unique ttl=1 values
    $p = str_repeat('p', 1000); $t = str_repeat('t', 256);
    for ($i = 0; $i < 50000; $i++) apcu_store("p$i", $p);
    $n = $fail = $slow = $max = 0;
    $end = hrtime(true) + 5e9;
    while (($now = hrtime(true)) < $end) {
        for ($i = 0; $i < 100; $i++, $n++) {
            $s = hrtime(true);
            $ok = $n & 1 ? apcu_store('p' . mt_rand(0, 49999), $p) : apcu_store("t$n", $t, 1);
            $d = hrtime(true) - $s;
            $fail += !$ok; $slow += $d > 50000; $max = max($max, $d);
        }
    }
    out('ttl churn: stores', $n / 5 / 1e6, 'M ops/s');
    out('ttl churn: failed stores', $fail, 'count');
    out('ttl churn: stores > 50us', $slow, 'count');
    out('ttl churn: slowest store', $max / 1e3, 'us');
    if (function_exists('nocopy_stats')) out('ttl churn: evacuated', (nocopy_stats()['evacuated_bytes'] ?? -1e6) / 1e6, 'MB');
}

if ($group === 'mixed') {
    // 4 readers on a 1k-row array while 1 writer rewrites it as fast as it can
    $a = rows(1000);
    apcu_store('hot', $a);
    apcu_store('stop', 0);
    $writer = pcntl_fork();
    if ($writer === 0) { $n = 0; while (!apcu_fetch('stop')) { apcu_store('hot', $a); $n++; } exit(0); }
    $ops = 3000; $t = hrtime(true); $pids = [];
    for ($k = 0; $k < 4; $k++) {
        $pid = pcntl_fork();
        if ($pid === 0) { for ($i = 0; $i < $ops; $i++) $x = apcu_fetch('hot'); exit(0); }
        $pids[] = $pid;
    }
    foreach ($pids as $p) pcntl_waitpid($p, $st);
    $secs = (hrtime(true) - $t) / 1e9;
    apcu_store('stop', 1);
    pcntl_waitpid($writer, $st);
    out('4 readers + 1 writer (reader throughput)', 4 * $ops / $secs / 1e3, 'k ops/s');
}
