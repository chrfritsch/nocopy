--TEST--
nocopy: equal short strings stored once per value, maps with a hash sized to their elements: same values, lookups, copy-on-write
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t072
--FILE--
<?php
require __DIR__ . '/serve.inc';
$test = <<<'CODE'
enum Suit: string { case Hearts = 'H'; case Spades = 'S'; }
class P { public $a; public $b; function __construct($a, $b) { $this->a = $a; $this->b = $b; } }
$long = str_repeat('L', 65);
$rows = [];
for ($i = 0; $i < 300; $i++) {
    $rows[] = ['lang' => 'en', 'status' => $i % 2 ? 'published' : 'draft', 'en' => 'lang', 'long' => $long, 'id' => "id$i", 'suit' => $i % 3 ? Suit::Hearts : Suit::Spades, 'p' => new P('en', 'draft')];
}
$holes = ['a' => 1, 'b' => 2, 'c' => 3, 'd' => 4, 'e' => 5];
unset($holes['b'], $holes['d']);
$big = [];
for ($i = 0; $i < 100; $i++) { $big["k$i"] = "v" . ($i % 7); }
unset($big['k5'], $big['k50']);
$vals = ['rows' => $rows, 'holes' => $holes, 'big' => $big, 'one' => ['only' => 'only'], 'mixed' => [5 => 'x', 'x' => 5, 'y' => 'x']];
foreach ($vals as $k => $v) {
    nocopy_put($k, $v);
    if (serialize(nocopy_get($k)) !== serialize($v)) echo "$k DIFFERS\n";
}
$h = nocopy_get('holes');
var_dump(isset($h['a'], $h['c'], $h['e']), isset($h['b']), array_key_last($h));
$b = nocopy_get('big');
$miss = 0;
for ($i = 0; $i < 100; $i++) { if (($b["k$i"] ?? null) !== ($big["k$i"] ?? null)) $miss++; }
var_dump($miss);
for ($i = 100; $i < 200; $i++) { $b["k$i"] = $i; }
var_dump(count($b), $b['k150'], $b['k99'], nocopy_get('big')['k99']);
$r = nocopy_get('rows');
var_dump($r[7]['status'], $r[7]['en'], $r[7]['suit'] === Suit::Hearts, $r[9]['suit'] === Suit::Spades, $r[7]['p']->a);
$r[7]['lang'] .= '-gb';
var_dump($r[7]['lang'], $r[8]['lang'], nocopy_get('rows')[7]['lang']);
$m = nocopy_get('mixed');
var_dump($m[5], $m['x'], $m['y']);
CODE;
echo "-- copy mode\n";
eval($test);
// a short string held 300 times is stored once
$u = nocopy_stats()['used'];
nocopy_put('strs', array_fill(0, 300, str_repeat('s', 40)));
var_dump(nocopy_stats()['used'] - $u < 300 * 40);
echo "-- zero-copy\n";
echo serve('<?php ' . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t072*')); ?>
--EXPECT--
-- copy mode
bool(true)
bool(false)
string(1) "e"
int(0)
int(198)
int(150)
string(2) "v1"
string(2) "v1"
string(9) "published"
string(4) "lang"
bool(true)
bool(true)
string(2) "en"
string(5) "en-gb"
string(2) "en"
string(2) "en"
string(1) "x"
int(5)
string(1) "x"
bool(true)
-- zero-copy
bool(true)
bool(false)
string(1) "e"
int(0)
int(198)
int(150)
string(2) "v1"
string(2) "v1"
string(9) "published"
string(4) "lang"
bool(true)
bool(true)
string(2) "en"
string(5) "en-gb"
string(2) "en"
string(2) "en"
string(1) "x"
int(5)
string(1) "x"
