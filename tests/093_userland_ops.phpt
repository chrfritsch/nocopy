--TEST--
nocopy: whatever PHP code does to a fetched value (modify, unset, references, sorts, json_encode, serialize, var_dump, extract, array_walk) leaves the arena's bytes and the cached value as they were
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t093
--FILE--
<?php
require __DIR__ . '/serve.inc';
$code = <<<'CODE'
<?php
enum E: string { case A = 'a'; case B = 'b'; }
class O { public $a = 'prop'; public $l = [3, 1, 2]; public $m = ['k' => 'v', 'n' => ['x' => 'y']]; public ?O $me = null; public $e = E::B; public $d; public $r; }
function values(): array {
    $plain = ['s' => 'str', 'n' => [1, 2, ['a' => 'b', 'c' => []]], 'p' => [3, 1, 2], 'e' => [], 'k' => 's', 'f' => 1.5, 'm' => ['str' => 'k', 'z' => null]];
    $o = new O; $o->me = $o; $o->d = new DateTimeImmutable('2026-01-02 03:04:05 UTC');
    $shared = ['q' => 'shared', 'w' => [1, 2]];
    $o->r = &$shared;
    return ['plain' => $plain, 'alone' => 'a string alone', 'graph' => $plain + ['o' => $o, 'ref' => &$shared, 'list' => [[5, 4], ['t' => 'u']], 'en' => E::A]];
}
function torture(mixed $c): void {
    if (is_string($c)) { $c[0] = 'X'; $c .= 'y'; $k = [$c => 1]; return; }
    $x = $c; $x['s'] = 'new'; $x['n'][2]['a'] .= 'x'; unset($x['n'][2]['c'], $x['p'][0], $x['e']); $x['n'][2]['c']['deep'][] = 1;
    $x = $c; $r = &$x['n'][0]; $r = 9; $r2 = &$x['m']['str']; $r2 .= 'z'; unset($r, $r2);
    $x = $c; foreach ($x as $k => &$v) { if (is_array($v)) { $v[] = 1; foreach ($v as &$w) { $w = 0; } unset($w); } else { $v = $k; } } unset($v);
    $x = $c; sort($x['p']); rsort($x['p']); ksort($x); asort($x['m']); usort($x['p'], fn($a, $b) => $b <=> $a); shuffle($x['p']); array_multisort($x['p']);
    $x = $c; array_walk($x, function (&$v, $k) { $v = $k; }); $x = $c; unset($x['o']); array_walk_recursive($x, function (&$v) { $v = 1; }); // not into the object: it holds itself
    $x = $c; array_push($x['p'], 1); array_pop($x['p']); array_shift($x['p']); array_unshift($x['p'], 0); array_splice($x['p'], 0, 1); end($x); reset($x); next($x); each_ref($x);
    (function () use ($c) { $s = 'old'; $n = 'old'; extract($c); extract($c, EXTR_REFS); $s = 1; $n[] = 2; })();
    json_encode($c); serialize($c); ob_start(); var_dump($c); print_r($c); var_export(is_array($c['n'] ?? 0) ? $c['n'] : 0); debug_zval_refcount: ob_end_clean();
    $x = $c; unset($x['o']); $x = array_merge_recursive($x, $x); $x = array_replace_recursive($x, ['n' => [2 => ['a' => 1]]]); $x = array_flip(['a' => $c['s'] ?? 'q']); $x = array_combine(array_keys($c), array_values($c));
    $x = $c; $y = &$x; $y['new'] = 1; unset($y);
    if (isset($c['o'])) {
        $o = $c['o']; $o->a .= 'x'; $o->a[0] = 'Z'; $o->l[] = 4; sort($o->l); unset($o->m['n']['x']); $o->m['n'][] = 1; $r = &$o->m['k']; $r = 2; unset($r);
        foreach ($o as $k => &$v) { if (is_string($v)) { $v .= '!'; } } unset($v);
        $a = (array) $o; $a['l'][] = 5; $cl = clone $o; $cl->l = []; json_encode($o->m); ob_start(); var_dump($o); ob_end_clean(); serialize($o);
        $c['ref'] = 'through the reference'; $o->r = null; $o->me = null;
        $c['list'][0][] = 6; sort($c['list'][0]);
    }
}
function each_ref(array &$a): void { foreach ($a as &$v) { } }
nocopy_get('plain', $stored);
if (!$stored) foreach (values() as $k => $v) nocopy_put($k, $v);
$file = glob('/dev/shm/nocopy-t093.*')[0];
foreach (values() as $k => $v) {
    $want = serialize(unserialize(serialize($v)));
    $a = nocopy_get($k); $witness = nocopy_get($k);
    $before = md5_file($file);
    torture($a); unset($a);
    $same = md5_file($file) === $before;
    echo PHP_SAPI === 'cli' ? 'copy' : 'zero', " $k: arena ", $same ? 'unchanged' : 'CHANGED', ', held value ', serialize($witness) === $want ? 'intact' : 'DIFFERS', ', fetched again ', serialize(nocopy_get($k)) === $want ? 'intact' : 'DIFFERS', "\n";
}
CODE;
$code = str_replace('debug_zval_refcount: ', '', $code);
echo serve($code);
file_put_contents($f = sys_get_temp_dir() . '/nocopy-t093.php', $code);
passthru(php_cmd(['nocopy.shm_name' => '/nocopy-t093'], [$f]));
unlink($f);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t093*')); ?>
--EXPECT--
zero plain: arena unchanged, held value intact, fetched again intact
zero alone: arena unchanged, held value intact, fetched again intact
zero graph: arena unchanged, held value intact, fetched again intact
copy plain: arena unchanged, held value intact, fetched again intact
copy alone: arena unchanged, held value intact, fetched again intact
copy graph: arena unchanged, held value intact, fetched again intact
