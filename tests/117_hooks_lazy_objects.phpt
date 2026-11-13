--TEST--
nocopy: property hooks, asymmetric visibility and lazy objects (PHP 8.4) round-trip as serialize() does
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t117
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
class Hooked {
    public string $full { get => $this->first . ' ' . $this->last; }
    public string $first { set(string $v) { $this->first = ucfirst($v); } }
    public array $log = [] { set(array $v) { $this->log = array_values($v); } }
    public function __construct(string $f, public private(set) string $last, protected(set) int $n = 1) { $this->first = $f; }
    public function bump(): void { $this->n++; }
}
class Ghost { public int $n = 0; public array $list = []; public function __construct(public string $s = 'x') {} }
CODE;
$test = <<<'CODE'
$h = new Hooked('ada', 'lovelace');
$h->log = ['k' => 'a', 'b'];
nocopy_put('h', ['one' => $h, 'two' => $h]);
for ($i = 0; $i < 2; $i++) {
    $r = nocopy_get('h');
    var_dump(serialize($r) === serialize(['one' => $h, 'two' => $h]), $r['one'] === $r['two'], $r['one']->full);
    $r['one']->first = 'grace';
    $r['one']->log = ['x' => 'y'];
    $r['one']->bump();
    var_dump($r['one']->full, $r['one']->log, $r['one']->n);
    try { $r['one']->last = 'z'; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $r['one']->n = 5; } catch (Error $e) { echo $e->getMessage(), "\n"; }
}
var_dump(serialize(nocopy_get('h')['one']) === serialize($h));

$rc = new ReflectionClass(Ghost::class);
$inits = 0;
$g = $rc->newLazyGhost(function (Ghost $o) use (&$inits) { $inits++; $o->__construct('init'); $o->n = 7; $o->list = [1, 2]; });
$want = serialize(['g' => $g]);
$inits = 0;
$g = $rc->newLazyGhost(function (Ghost $o) use (&$inits) { $inits++; $o->__construct('init'); $o->n = 7; $o->list = [1, 2]; });
nocopy_put('g', ['g' => $g]);
$gr = nocopy_get('g')['g'];
var_dump($inits, $rc->isUninitializedLazyObject($g), $rc->isUninitializedLazyObject($gr), serialize(['g' => $gr]) === $want);
$gr->list[] = 3;
var_dump(nocopy_get('g')['g']->list);

$p = $rc->newLazyProxy(fn() => new Ghost('proxied'));
nocopy_put('p', $p);
var_dump(serialize(nocopy_get('p')) === serialize($p), nocopy_get('p')->s);

$u = $rc->newLazyGhost(fn(Ghost $o) => $o->__construct('u'), ReflectionClass::SKIP_INITIALIZATION_ON_SERIALIZE);
nocopy_put('u', $u);
$ur = nocopy_get('u');
var_dump($rc->isUninitializedLazyObject($u), serialize($ur) === serialize(unserialize(serialize($u))), (new ReflectionProperty(Ghost::class, 's'))->isInitialized($ur));
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t117*')); ?>
--EXPECT--
-- copy mode
bool(true)
bool(true)
string(12) "Ada lovelace"
string(14) "Grace lovelace"
array(1) {
  [0]=>
  string(1) "y"
}
int(2)
Cannot modify private(set) property Hooked::$last from global scope
Cannot modify protected(set) property Hooked::$n from global scope
bool(true)
bool(true)
string(12) "Ada lovelace"
string(14) "Grace lovelace"
array(1) {
  [0]=>
  string(1) "y"
}
int(2)
Cannot modify private(set) property Hooked::$last from global scope
Cannot modify protected(set) property Hooked::$n from global scope
bool(true)
int(1)
bool(false)
bool(false)
bool(true)
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
bool(true)
string(7) "proxied"
bool(true)
bool(true)
bool(false)
-- zero-copy
bool(true)
bool(true)
string(12) "Ada lovelace"
string(14) "Grace lovelace"
array(1) {
  [0]=>
  string(1) "y"
}
int(2)
Cannot modify private(set) property Hooked::$last from global scope
Cannot modify protected(set) property Hooked::$n from global scope
bool(true)
bool(true)
string(12) "Ada lovelace"
string(14) "Grace lovelace"
array(1) {
  [0]=>
  string(1) "y"
}
int(2)
Cannot modify private(set) property Hooked::$last from global scope
Cannot modify protected(set) property Hooked::$n from global scope
bool(true)
int(1)
bool(false)
bool(false)
bool(true)
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
bool(true)
string(7) "proxied"
bool(true)
bool(true)
bool(false)
