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
final class Cat { public function __construct(public int $id, public string $name) {} }
class Prod { private array $meta = ['source' => 'import', 'flags' => [1, 2, 3]];
    public function __construct(public int $id, public string $name, protected string $desc, public array $tags, public ?Cat $cat, public float $price) {} }
enum St: string { case Open = 'o'; case Paid = 'p'; }
final class Ord { public function __construct(public int $id, public DateTimeImmutable $at, public St $status, public string $note) {} }
$cats = [new Cat(1, 'Books'), new Cat(2, 'Music'), new Cat(3, 'Games')];
$g = [
    'prods' => array_map(fn($i) => new Prod($i, "Product $i", str_repeat("Description of product $i. ", 4), ['a', 'b', "t$i"], $cats[$i % 3], $i * 1.5), range(1, 100)),
    'ords' => array_map(fn($i) => new Ord($i, new DateTimeImmutable("2026-01-01 +$i hours"), $i % 2 ? St::Open : St::Paid, "note $i"), range(1, 100)),
    'json' => json_decode(json_encode(array_map(fn($i) => ['id' => $i, 'name' => "Product $i", 'tags' => ['a', "t$i"], 'cat' => ['id' => $i % 3, 'name' => 'Books']], range(1, 100)))),
];
foreach ($g as $gk => $gv) nocopy_put($gk, $gv);
$k = []; for ($i = 0; $i < 10; $i++) { $k[] = "m$i"; nocopy_put("m$i", $i); }
$d = []; for ($i = 0; $i < 100000; $i++) { $d[] = "d$i"; nocopy_put("d$i", $i); }
$per = ['get_obj' => 20, 'get_multi' => 10, 'put_arr' => 10, 'put_big' => 1000, 'put_obj' => 10, 'put_del' => 2, 'put_100k' => 2, 'get_prods' => 1000, 'get_ords' => 1000, 'get_json' => 1000, 'put_prods' => 1000, 'put_ords' => 1000, 'put_json' => 1000, 'add_del' => 2, 'putdel_multi' => 2, 'put_multi' => 10, 'delete_multi_miss' => 10, 'keys' => 1, 'keys_100k' => 1000, 'stats' => 100, 'clear_put' => 2, 'remember_miss' => 2][$op] ?? 1;
if (($argv[2] ?? '') === 'per') exit((string) $per); // n / loop count
$pm = array_combine(array_map(fn($i) => "pm$i", range(0, 9)), range(0, 9)); $miss = array_map(fn($i) => "nx$i", range(0, 9)); nocopy_put('cn', 0);
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
    case 'get_prods': case 'get_ords': case 'get_json': $gk = substr($op, 4); for ($i = 0; $i < $n / 1000; $i++) $x = nocopy_get($gk); break;
    case 'put_prods': case 'put_ords': case 'put_json': $gv = $g[substr($op, 4)]; for ($i = 0; $i < $n / 1000; $i++) nocopy_put('wg', $gv); break;
    case 'inc':       for ($i = 0; $i < $n; $i++) nocopy_inc('c'); break;
    case 'add_fail':  for ($i = 0; $i < $n; $i++) nocopy_add('int', 1); break;
    case 'get_hit':   for ($i = 0; $i < $n; $i++) $x = nocopy_get('int', $hit); break;
    case 'inc_ttl':   for ($i = 0; $i < $n; $i++) nocopy_inc('c', 1, 60); break;
    case 'dec':       for ($i = 0; $i < $n; $i++) nocopy_inc('c', -1); break;
    case 'add_del':   for ($i = 0; $i < $n / 2; $i++) { nocopy_add('wd', $i); nocopy_delete('wd'); } break;
    case 'cas_ok':    for ($i = 0; $i < $n; $i++) nocopy_cas('cn', $i, $i + 1); break;
    case 'cas_fail':  for ($i = 0; $i < $n; $i++) nocopy_cas('cn', -1, 0); break;
    case 'delete_miss': for ($i = 0; $i < $n; $i++) nocopy_delete('nope'); break;
    case 'delete_multi_miss': for ($i = 0; $i < $n / 10; $i++) nocopy_delete_multiple($miss); break;
    case 'put_multi': for ($i = 0; $i < $n / 10; $i++) nocopy_put_multiple($pm); break;
    case 'putdel_multi': for ($i = 0; $i < $n / 2; $i++) { nocopy_put_multiple($pm); nocopy_delete_multiple(array_keys($pm)); } break;
    case 'keys':      for ($i = 0; $i < $n; $i++) $x = nocopy_keys('m'); break;
    case 'keys_100k': for ($i = 0; $i < $n / 1000; $i++) $x = nocopy_keys('d'); break;
    case 'stats':     for ($i = 0; $i < $n / 100; $i++) $x = nocopy_stats(); break;
    case 'shared_with': for ($i = 0; $i < $n; $i++) $x = nocopy_shared_with(); break;
    case 'clear_put': for ($i = 0; $i < $n / 2; $i++) { nocopy_put('wd', $i); nocopy_clear(); } break;
    case 'remember_miss': $f = fn() => 1; for ($i = 0; $i < $n / 2; $i++) { nocopy_delete('rm'); nocopy_remember('rm', $f); } break;
    default: exit("ops: get_int get_ttl get_str get_arr get_big get_obj get_miss get_100k get_multi remember put_int put_ttl put_str put_arr put_big put_obj put_del put_100k get_prods get_ords get_json put_prods put_ords put_json inc add_fail\n");
}
printf("%-10s %-4s %9.2f ns/op\n", $op, $zero ? 'zero' : 'copy', (hrtime(true) - $t) / $n * $per);
