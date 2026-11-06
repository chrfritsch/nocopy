--TEST--
nocopy: plain objects come back as their property slots (identity, cycles, inheritance, readonly, uninitialized), others serialized; a changed class is a miss
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t067
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
class Base { private string $secret = 'base'; protected array $tags = ['t']; }
final class Cat { public function __construct(public int $id, public string $name) {} }
class Product extends Base {
    public ?Product $self = null;
    public int $late;
    public function __construct(public readonly int $id, public string $name, public array $list, public ?Cat $cat) {}
}
class Wakes { public $n = 0; public function __wakeup() { $this->n++; } }
class Ser { public $a = 1; public function __serialize(): array { return ['a' => 2]; } public function __unserialize(array $d): void { $this->a = $d['a']; } }
#[AllowDynamicProperties] class Dyn {}
enum Suit: string { case Hearts = 'h'; }
CODE;
$test = <<<'CODE'
$cat = new Cat(1, 'Books');
$p = new Product(7, 'P', [1, ['x' => 'y'], 'z'], $cat);
$p->self = $p;
$d = new Dyn; $d->x = 1;
$vals = [
    'graph' => [$p, new Product(8, 'Q', [], $cat), $cat],
    'mixed' => ['rows' => array_fill(0, 3, ['k' => str_repeat('v', 40)]), 'o' => $cat],
    'ser' => new Ser,
    'std' => (object) ['a' => 1],
    'dyn' => $d,
    'date' => [new DateTimeImmutable('2026-01-02'), $cat],
    'enum' => [Suit::Hearts, $cat],
];
foreach ($vals as $k => $v) {
    nocopy_put($k, $v);
    $got = nocopy_get($k);
    echo $k, ': ', serialize($got) === serialize($v) ? 'same' : 'DIFFERS', "\n";
}
$g = nocopy_get('graph');
var_dump($g[0]->self === $g[0], $g[0]->cat === $g[1]->cat, $g[1]->cat === $g[2]);
var_dump((new ReflectionProperty(Product::class, 'late'))->isInitialized($g[0]));
try { $g[0]->id = 9; } catch (Error $e) { echo get_class($e), "\n"; }
$g[0]->list[1]['x'] = 'changed';
$g[2]->name = 'changed';
var_dump(nocopy_get('graph')[0]->list[1]['x'], nocopy_get('graph')[2]->name);
for ($i = 0; $i < 4; $i++) { $g = nocopy_get('graph'); $g[0]->list[1]['x'] .= '!'; $g[0]->list[] = $i; $g[] = 'extra'; $m = nocopy_get('mixed'); $m['rows'][0]['k'] .= '!'; }
var_dump($g[0]->list[1]['x'], count($g[0]->list), count($g), $m['rows'][0]['k'] === str_repeat('v', 40) . '!');
nocopy_put('g2', $vals['graph']);
nocopy_put('graph', [new Cat(5, 'new')]);
var_dump(nocopy_get('graph')[0]->name, nocopy_get('graph')[0]->name);
nocopy_put('wakes', new Wakes);
var_dump(nocopy_get('wakes')->n);
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test . <<<'CODE'
ob_start(); debug_zval_dump(nocopy_get('mixed')['rows']); echo str_contains(ob_get_clean(), 'interned') ? "rows shared\n" : "rows copied\n";
CODE);
echo "-- autoloaded, re-entrant\n";
echo serve('<?php spl_autoload_register(function ($c) { nocopy_put("side", [$c]); nocopy_get("mixed"); if (!class_exists("Base", false)) eval(' . var_export($classes, true) . '); }); $g = nocopy_get("g2"); var_dump($g[0]->self === $g[0], $g[2]->name, nocopy_get("side"));');
echo "-- class changed\n";
echo serve('<?php class Cat { public function __construct(public string $id, public string $name) {} } var_dump(nocopy_get("mixed", $hit), $hit);');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t067*')); ?>
--EXPECT--
-- copy mode
graph: same
mixed: same
ser: same
std: same
dyn: same
date: same
enum: same
bool(true)
bool(true)
bool(true)
bool(false)
Error
string(1) "y"
string(5) "Books"
string(2) "y!"
int(4)
int(4)
bool(true)
string(3) "new"
string(3) "new"
int(1)
-- zero-copy
graph: same
mixed: same
ser: same
std: same
dyn: same
date: same
enum: same
bool(true)
bool(true)
bool(true)
bool(false)
Error
string(1) "y"
string(5) "Books"
string(2) "y!"
int(4)
int(4)
bool(true)
string(3) "new"
string(3) "new"
int(1)
rows shared
-- autoloaded, re-entrant
bool(true)
string(5) "Books"
array(1) {
  [0]=>
  string(3) "Cat"
}
-- class changed
NULL
bool(false)
