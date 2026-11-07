<?php
// nocopy vs values kept as PHP files in OPcache (what Symfony's PhpFilesAdapter / PhpArrayAdapter do), both zero-copy.
// file_update_protection=0: fresh files are cached at once (OPcache's best case).
// usage: NOCOPY_SO=... php [-d nocopy.*] opcache.php    (reruns itself as one php -S request with OPcache on)
// emits JSON lines like run.php.

if (PHP_SAPI !== 'cli-server') {
    require __DIR__ . '/../tests/serve.inc';
    echo serve('<?php require ' . var_export(__FILE__, true) . ';', ['opcache.enable' => 1, 'opcache.validate_timestamps' => 0,
        'opcache.memory_consumption' => 256, 'opcache.max_accelerated_files' => 100000, 'opcache.max_wasted_percentage' => 50, 'opcache.file_update_protection' => 0]);
    exit;
}

function out(string $name, float $v, string $unit): void {
    echo json_encode(['name' => $name, 'v' => round($v, 3), 'unit' => $unit]), "\n";
}

function bench(string $name, callable $fn): void {
    $n = 100;
    do { $n *= 2; $t = hrtime(true); $fn($n); $e = hrtime(true) - $t; } while ($e < 20e6);
    $n = (int) ($n * 120e6 / $e) + 1;
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

// PhpFilesAdapter's write: export, temp file + rename, invalidate; the next include compiles it
function file_put(string $f, mixed $v): void {
    file_put_contents("$f.tmp", '<?php return ' . var_export($v, true) . ';');
    rename("$f.tmp", $f);
    opcache_invalidate($f, true);
}

$dir = sys_get_temp_dir() . '/opc-' . getmypid();
mkdir($dir);
$big = rows(1000);
$one = rows(1);
file_put("$dir/big.php", $big); include "$dir/big.php";
nocopy_put('big', $big);
for ($i = 0; $i < 1000; $i++) { file_put("$dir/k$i.php", $one); include "$dir/k$i.php"; nocopy_put("k$i", $one); }
if (!opcache_is_script_cached("$dir/big.php")) { echo "opcache not caching\n"; exit(1); }

bench('fetch array 1k rows: opcache file', function ($n) use ($dir) { for ($i = 0; $i < $n; $i++) $x = include "$dir/big.php"; });
bench('fetch array 1k rows: nocopy', function ($n) { for ($i = 0; $i < $n; $i++) $x = nocopy_get('big'); });
bench('fetch 1 of 1000 keys: opcache file', function ($n) use ($dir) { for ($i = 0; $i < $n; $i++) $x = include "$dir/k" . ($i % 1000) . '.php'; });
bench('fetch 1 of 1000 keys: nocopy', function ($n) { for ($i = 0; $i < $n; $i++) $x = nocopy_get('k' . ($i % 1000)); });
bench('store 1-row array: opcache file (+ first include)', function ($n) use ($dir, $one) { for ($i = 0; $i < $n; $i++) { file_put("$dir/w.php", $one); $x = include "$dir/w.php"; } });
bench('store 1-row array: nocopy', function ($n) use ($one) { for ($i = 0; $i < $n; $i++) nocopy_put('w', $one); });
bench('store array 1k rows: opcache file (+ first include)', function ($n) use ($dir, $big) { for ($i = 0; $i < $n; $i++) { file_put("$dir/wb.php", $big); $x = include "$dir/wb.php"; } });
bench('store array 1k rows: nocopy', function ($n) use ($big) { for ($i = 0; $i < $n; $i++) nocopy_put('wb', $big); });

// a rewritten file's old script stays in OPcache as wasted memory until OPcache restarts (all scripts recompiled)
$w0 = opcache_get_status(false)['memory_usage']['wasted_memory'];
for ($i = 0; $i < 100; $i++) { file_put("$dir/wb.php", $big); include "$dir/wb.php"; }
out('opcache wasted per rewrite, 1k rows', (opcache_get_status(false)['memory_usage']['wasted_memory'] - $w0) / 100, 'bytes');
out('opcache restarts during the run', opcache_get_status(false)['opcache_statistics']['oom_restarts'] + opcache_get_status(false)['opcache_statistics']['hash_restarts'] + opcache_get_status(false)['opcache_statistics']['manual_restarts'], 'count');
array_map('unlink', glob("$dir/*")); rmdir($dir);
