<?php
// php [-d nocopy.*] prof.php <op> [zero] [n]: one hot loop per op, for perf (docker/perf.sh). zero: as one php -S request. Needs NOCOPY_SO for zero.
$op = $argv[1] ?? 'list'; $zero = ($argv[2] ?? '') === 'zero'; $n = (int) ($argv[3] ?? 20000000);
if ($zero && PHP_SAPI === 'cli') {
    require __DIR__ . '/../tests/serve.inc';
    echo serve('<?php $argv = [0, ' . var_export($op, true) . ', "zero", ' . $n . ']; require ' . var_export(__FILE__, true) . ';', ['extension' => 'pcntl']);
    exit;
}
$rows = fn($k) => array_map(fn($i) => ['id' => $i, 'name' => "n$i", 'score' => $i * 1.5], range(1, $k));
$r10 = $rows(10); $r1k = $rows(1000); $s100 = str_repeat('x', 100);
nocopy_put('int', 42); nocopy_put('str', $s100); nocopy_put('arr', $r10); nocopy_put('big', $r1k);
nocopy_put('ttl', 42, 3600); nocopy_put('obj', new ArrayObject([1, 2, 3]));
$k = []; for ($i = 0; $i < 10; $i++) { $k[] = "m$i"; nocopy_put("m$i", $i); }
$d = []; for ($i = 0; $i < 100000; $i++) { $d[] = "d$i"; nocopy_put("d$i", $i); }
$per = ['get_obj' => 20, 'get_multi' => 10, 'put_arr' => 10, 'put_big' => 1000, 'put_obj' => 10, 'put_del' => 2, 'put_100k' => 2][$op] ?? 1; // n / loop count
$t = hrtime(true);
switch ($op) {
    case 'get_int':   for ($i = 0; $i < $n; $i++) $x = nocopy_get('int'); break;
    case 'get_ttl':   for ($i = 0; $i < $n; $i++) $x = nocopy_get('ttl'); break;
    case 'get_str':   for ($i = 0; $i < $n; $i++) $x = nocopy_get('str'); break;
    case 'get_arr':   for ($i = 0; $i < $n; $i++) $x = nocopy_get('arr'); break;
    case 'get_big':   for ($i = 0; $i < $n; $i++) $x = nocopy_get('big'); break;
    case 'get_obj':   for ($i = 0; $i < $n / 20; $i++) $x = nocopy_get('obj'); break;
    case 'get_miss':  for ($i = 0; $i < $n; $i++) $x = nocopy_get('nope'); break;
    case 'get_100k':  for ($i = 0; $i < $n; $i++) $x = nocopy_get($d[$i % 100000]); break;
    case 'get_multi': for ($i = 0; $i < $n / 10; $i++) $x = nocopy_get_multiple($k); break;
    case 'remember':  $f = fn() => 1; for ($i = 0; $i < $n; $i++) $x = nocopy_remember('int', $f); break;
    case 'put_int':   for ($i = 0; $i < $n; $i++) nocopy_put('w', $i); break;
    case 'put_ttl':   for ($i = 0; $i < $n; $i++) nocopy_put('w', $i, 60); break;
    case 'put_str':   for ($i = 0; $i < $n; $i++) nocopy_put('w', $s100); break;
    case 'put_arr':   for ($i = 0; $i < $n / 10; $i++) nocopy_put('wa', $r10); break;
    case 'put_big':   for ($i = 0; $i < $n / 1000; $i++) nocopy_put('wb', $r1k); break;
    case 'put_obj':   $o = new ArrayObject([1, 2, 3]); for ($i = 0; $i < $n / 10; $i++) nocopy_put('wo', $o); break;
    case 'put_del':   for ($i = 0; $i < $n / 2; $i++) { nocopy_put('wd', $i); nocopy_delete('wd'); } break;
    case 'put_100k':  for ($i = 0; $i < $n / 2; $i++) nocopy_put($d[$i % 100000], $i); break;
    case 'inc':       for ($i = 0; $i < $n; $i++) nocopy_inc('c'); break;
    case 'add_fail':  for ($i = 0; $i < $n; $i++) nocopy_add('int', 1); break;
    default: exit("ops: get_int get_ttl get_str get_arr get_big get_obj get_miss get_100k get_multi remember put_int put_ttl put_str put_arr put_big put_obj put_del put_100k inc add_fail\n");
}
printf("%-10s %-4s %9.2f ns/op\n", $op, $zero ? 'zero' : 'copy', (hrtime(true) - $t) / $n * $per);
