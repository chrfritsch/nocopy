--TEST--
nocopy: classes that redeclare inherited properties (slot holes, as Drupal's FieldConfig) are stored as slots, not serialized
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t087
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
class P { public $a = 1; protected $b = 2; private $c = 3; public function c() { return $this->c; } }
class C extends P { public $a = 10; protected $b = 20; public int $d = 4; public function b() { return $this->b; } }
class S extends C { public $e = 5; public function __sleep() { return ['a', 'd', 'b']; } }
CODE;
eval($classes);
$c = new C; $c->a = 'x'; $c->d = 7;
$s = new S; $s->a = 'y'; $s->e = 'dropped';
nocopy_put('v', [$c, $s, $c]);
$g = nocopy_get('v');
var_dump($g[0]->a, $g[0]->b(), $g[0]->c(), $g[0]->d, $g[0] === $g[2], $g[1]->a, $g[1]->e, $g[1]->b(), $g == [$c, (function () { $x = new S; $x->a = 'y'; return $x; })(), $c]);
echo serve('<?php ' . $classes . ' $g = nocopy_get("v"); var_dump($g[0]->a, $g[1]->b(), $g[0] === $g[2]);');
// stored as slots: a class whose layout changed is a miss (a serialized value would still unserialize)
echo serve('<?php class P { public $a = 1; protected $b = 2; private $c = 3; } class C extends P { public int $d = 4; } class S extends C { public $e = 5; } var_dump(nocopy_get("v", $hit), $hit);');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t087*')); ?>
--EXPECT--
string(1) "x"
int(20)
int(3)
int(7)
bool(true)
string(1) "y"
int(5)
int(20)
bool(true)
string(1) "x"
int(20)
bool(true)
NULL
bool(false)
