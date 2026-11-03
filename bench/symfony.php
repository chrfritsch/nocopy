<?php
// Benchmarks Symfony Cache's ApcuAdapter (the real consumer of apcu_*).
// usage: SF_AUTOLOAD=/path/to/symfony-cache/vendor/autoload.php php [-d ...] symfony.php <group>   groups: read, write, parallel

require getenv('SF_AUTOLOAD') ?: exit("set SF_AUTOLOAD\n");
use Symfony\Component\Cache\Adapter\ApcuAdapter;

function out(string $name, float $v, string $unit): void { echo json_encode(['name' => $name, 'v' => round($v, 3), 'unit' => $unit]), "\n"; }

function bench(string $name, callable $fn): void {
    $n = 100;
    do { $n *= 2; $t = hrtime(true); $fn($n); $e = hrtime(true) - $t; } while ($e < 20e6);
    $n = (int) ($n * 120e6 / $e) + 1;
    $fn((int) ($n / 10) + 1);
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

class Node { public $name = 'n'; public $meta = ['a' => 1, 'b' => [1, 2, 3]]; public ?Node $next = null; }

if (!apcu_enabled() || ini_get('apc.shm_size') !== '512M') { fwrite(STDERR, "bench misconfigured\n"); exit(1); }
$group = $argv[1] ?? 'read';
$cache = new ApcuAdapter('bench', 0);

if ($group === 'read') {
    $obj = new Node; $obj->next = new Node;
    $cache->get('scalar', fn() => 42);
    $cache->get('rows100', fn() => rows(100));
    $cache->get('rows10k', fn() => rows(10000));
    $cache->get('object', fn() => $obj);
    foreach (range(0, 9) as $i) $cache->get("m$i", fn() => rows(100));
    $ids = array_map(fn($i) => "m$i", range(0, 9));

    bench('Symfony get() hit: scalar', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) $x = $cache->get('scalar', fn() => 0); });
    bench('Symfony get() hit: 100-row array', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) $x = $cache->get('rows100', fn() => 0); });
    bench('Symfony get() hit: 10k-row array', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) $x = $cache->get('rows10k', fn() => 0); });
    bench('Symfony get() hit: 10k rows + read 1', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) { $x = $cache->get('rows10k', fn() => 0); $y = $x['key9999']['name']; } });
    bench('Symfony get() hit: object graph', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) $x = $cache->get('object', fn() => 0); });
    bench('Symfony get() miss (callback builds scalar)', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) { $cache->deleteItem('tmp'); $x = $cache->get('tmp', fn() => 1); } });
    bench('Symfony getItems() 10 x 100 rows', function ($n) use ($cache, $ids) { for ($i = 0; $i < $n; $i++) foreach ($cache->getItems($ids) as $it) $x = $it->get(); });
    bench('Symfony hasItem()', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) $x = $cache->hasItem('scalar'); });
}

if ($group === 'write') {
    $r100 = rows(100); $r10k = rows(10000);
    bench('Symfony save(): scalar', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) { $it = $cache->getItem('w1'); $it->set($i); $cache->save($it); } });
    bench('Symfony save(): 100-row array', function ($n) use ($cache, $r100) { for ($i = 0; $i < $n; $i++) { $it = $cache->getItem('w2'); $it->set($r100); $cache->save($it); } });
    bench('Symfony save(): 10k-row array', function ($n) use ($cache, $r10k) { for ($i = 0; $i < $n; $i++) { $it = $cache->getItem('w3'); $it->set($r10k); $cache->save($it); } });
    bench('Symfony deleteItem()', function ($n) use ($cache) { for ($i = 0; $i < $n; $i++) $cache->deleteItem('w1'); });
}

if ($group === 'parallel') {
    $cache->get('rows100', fn() => rows(100));
    foreach ([1, 2, 4, 8] as $w) {
        $ops = 20000; $t = hrtime(true); $pids = [];
        for ($k = 0; $k < $w; $k++) {
            $pid = pcntl_fork();
            if ($pid === 0) { for ($i = 0; $i < $ops; $i++) $x = $cache->get('rows100', fn() => 0); exit(0); }
            $pids[] = $pid;
        }
        foreach ($pids as $p) pcntl_waitpid($p, $st);
        out("Symfony get() hit 100 rows x$w workers", $w * $ops / ((hrtime(true) - $t) / 1e9) / 1e3, 'k ops/s');
    }
}
