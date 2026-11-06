--TEST--
nocopy: __wakeup, __serialize/__unserialize (user and internal classes) and enums as graph nodes: same results and hook order as unserialize(), throwing hooks, re-entrant __serialize
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t068
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
$GLOBALS['log'] = [];
class W { public ?W $child = null; public function __construct(public string $name) {} public function __wakeup() { $GLOBALS['log'][] = "w:$this->name"; } }
class S {
    public $o, $d;
    public function __serialize(): array { return ['o' => $this->o, 'd' => $this->d]; }
    public function __unserialize(array $a): void { $GLOBALS['log'][] = 'u:' . $a['d']; $this->o = $a['o']; $this->d = $a['d']; }
}
class Boom { public function __serialize(): array { return []; } public function __unserialize(array $a): void { throw new RuntimeException('boom'); } }
class BadSer { public function __serialize(): array { throw new LogicException('no'); } public function __unserialize(array $a): void {} }
class NotArr { public function __serialize() { return 1; } public function __unserialize(array $a): void {} }
class Puts { public $rows; public function __serialize(): array { nocopy_put('side', ['alpha' => ['beta' => 1, 'gamma' => 2]]); return ['rows' => $this->rows]; } public function __unserialize(array $a): void { $this->rows = $a['rows']; } }
final class Cat { public function __construct(public int $id, public string $name) {} }
enum Suit: string { case Hearts = 'h'; case Spades = 's'; }
CODE;
$test = <<<'CODE'
$cat = new Cat(1, 'Books');
$a = new W('a'); $a->child = new W('b'); $a->child->child = new W('c');
$s1 = new S; $s1->d = 'outer'; $s1->o = new S; $s1->o->d = 'inner'; $s1->o->o = $cat;
$st = new SplObjectStorage; $st[$cat] = 'data';
$p = new Puts; $p->rows = ['alpha' => ['beta' => 'x'], 'gamma' => ['alpha' => 'y']];
$vals = [
    'wake' => [$a, $a->child],
    'ser' => [$s1, $cat],
    'date' => [new DateTimeImmutable('2026-01-02 03:04:05', new DateTimeZone('Europe/Berlin')), $cat],
    'spl' => [$st, $cat, new ArrayObject(['k' => $cat])],
    'enum' => [Suit::Hearts, Suit::Spades, Suit::Hearts, $cat],
    'puts' => [$p, ['alpha' => 1, 'beta' => ['gamma' => 2]]],
];
foreach ($vals as $k => $v) {
    nocopy_put($k, $v);
    for ($i = 0; $i < 3; $i++) { // 3: copy mode keeps a template from the second fetch on
        $GLOBALS['log'] = []; $want = unserialize(serialize($v)); $wlog = $GLOBALS['log'];
        $GLOBALS['log'] = []; $got = nocopy_get($k); $glog = $GLOBALS['log'];
        if (serialize($got) !== serialize($want) || $glog !== $wlog) echo "$k #$i DIFFERS ", json_encode([$glog, $wlog]), "\n";
    }
    echo "$k: ", implode(',', $glog) ?: '-', "\n";
}
$g = nocopy_get('spl');
var_dump(isset($g[0][$g[1]]), $g[2]['k'] === $g[1], nocopy_get('enum')[0] === Suit::Hearts, nocopy_get('date')[0]->format('c'));
var_dump(nocopy_get('side'));
nocopy_put('boom', [new Boom, $cat]);
for ($i = 0; $i < 3; $i++) { try { var_dump(nocopy_get('boom', $hit), $hit); } catch (RuntimeException $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; } }
foreach ([new BadSer, new NotArr] as $o) { try { var_dump(nocopy_put('bad', [$o])); } catch (Throwable $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; } }
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
echo "-- enum case gone\n";
echo serve('<?php enum Suit: string { case Hearts = "h"; } final class Cat { public function __construct(public int $id, public string $name) {} } var_dump(nocopy_get("enum", $hit), $hit);');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t068*')); ?>
--EXPECTF--
-- copy mode
wake: w:c,w:b,w:a
ser: u:inner,u:outer
date: -
spl: -
enum: -
puts: -
bool(true)
bool(true)
bool(true)
string(25) "2026-01-02T03:04:05+01:00"
array(1) {
  ["alpha"]=>
  array(2) {
    ["beta"]=>
    int(1)
    ["gamma"]=>
    int(2)
  }
}
RuntimeException: boom
RuntimeException: boom
RuntimeException: boom
LogicException: no
TypeError: NotArr::__serialize() must return an array
-- zero-copy
wake: w:c,w:b,w:a
ser: u:inner,u:outer
date: -
spl: -
enum: -
puts: -
bool(true)
bool(true)
bool(true)
string(25) "2026-01-02T03:04:05+01:00"
array(1) {
  ["alpha"]=>
  array(2) {
    ["beta"]=>
    int(1)
    ["gamma"]=>
    int(2)
  }
}
RuntimeException: boom
RuntimeException: boom
RuntimeException: boom
LogicException: no
TypeError: NotArr::__serialize() must return an array
-- enum case gone
NULL
bool(false)
