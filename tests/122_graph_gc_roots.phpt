--TEST--
nocopy: storing and fetching objects without hooks adds no possible GC roots (as unserialize() adds none); cycles fetched are still collected
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t122
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
final class Cat { public function __construct(public int $id, public string $name) {} }
class Prod { public ?Prod $peer = null; public function __construct(public int $id, public array $tags, public ?Cat $cat) {} }
class Wakes { public $o; public function __wakeup() {} }
CODE;
$test = <<<'CODE'
$roots = fn() => gc_status()['roots'];
gc_collect_cycles();
$cat = new Cat(1, 'Books');
$v = array_map(fn($i) => new Prod($i, ['a', "t$i"], $cat), range(1, 500));
$shared = [1, 2, 3];
$refs = ['a' => &$shared, 'b' => &$shared, 'o' => $v[0]];
gc_collect_cycles(); // what building them noted
$roots();
$n = $roots();
nocopy_put('g', $v);
nocopy_put('r', $refs);
echo "put: ", $roots() - $n, "\n";
$keep = [];
for ($i = 0; $i < 4; $i++) {
    $keep[] = nocopy_get('g');
    $keep[] = nocopy_get('r');
}
echo "get: ", $roots() - $n, "\n";
var_dump($keep[0] == $v, $keep[6][499]->cat === $keep[6][0]->cat, $keep[7]['a'] === [1, 2, 3]);
$keep[7]['a'][] = 4;
var_dump($keep[7]['b'] === [1, 2, 3, 4]);

// a cycle: nothing notes it while it is in use; dropping it does, and the collector frees both objects
$a = new Prod(1, [], null); $b = new Prod(2, [], null); $a->peer = $b; $b->peer = $a;
nocopy_put('cyc', $a);
unset($a, $b);
gc_collect_cycles();
for ($i = 0; $i < 3; $i++) {
    $x = nocopy_get('cyc');
    var_dump($x->peer->peer === $x);
    unset($x);
    echo "collected: ", gc_collect_cycles(), "\n";
}

// hooks run user code: those graphs are released as before
$w = new Wakes; $w->o = new Wakes;
nocopy_put('w', $w);
$x = nocopy_get('w');
var_dump($x == $w, $x->o instanceof Wakes);
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t122*')); ?>
--EXPECT--
-- copy mode
put: 0
get: 0
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
collected: 2
bool(true)
collected: 2
bool(true)
collected: 2
bool(true)
bool(true)
-- zero-copy
put: 0
get: 0
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
collected: 2
bool(true)
collected: 2
bool(true)
collected: 2
bool(true)
bool(true)
