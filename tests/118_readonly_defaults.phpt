--TEST--
nocopy: readonly properties with default values (PHP 8.6) round-trip as serialize() does, also when __sleep() leaves them out
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (PHP_VERSION_ID < 80600) die('skip readonly defaults need PHP 8.6'); ?>
--INI--
nocopy.shm_name=/nocopy-t118
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
class R { public readonly int $a = 5; public readonly array $b = ['x', ['y']]; public function __construct(public readonly string $c = 'c') {} }
class S { public readonly int $a = 5; public readonly array $b = ['d']; public int $z = 1; public function __sleep(): array { return ['z']; } }
CODE;
$test = <<<'CODE'
$s = new S; $s->z = 2;
$vals = ['r' => new R('given'), 's' => $s];
nocopy_put('v', $vals);
for ($i = 0; $i < 2; $i++) {
    $v = nocopy_get('v');
    var_dump(serialize($v) === serialize($vals), $v['r']->a, $v['r']->b[1][0], $v['r']->c, $v['s']->b, $v['s']->z);
    try { $v['r']->a = 6; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    try { $v['s']->b[] = 'e'; } catch (Error $e) { echo $e->getMessage(), "\n"; }
    $c = clone $v['r'];
    var_dump($c == $v['r']);
}
CODE;
echo "-- copy mode\n";
eval($classes . $test);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t118*')); ?>
--EXPECT--
-- copy mode
bool(true)
int(5)
string(1) "y"
string(5) "given"
array(1) {
  [0]=>
  string(1) "d"
}
int(2)
Cannot modify readonly property R::$a
Cannot indirectly modify readonly property S::$b
bool(true)
bool(true)
int(5)
string(1) "y"
string(5) "given"
array(1) {
  [0]=>
  string(1) "d"
}
int(2)
Cannot modify readonly property R::$a
Cannot indirectly modify readonly property S::$b
bool(true)
-- zero-copy
bool(true)
int(5)
string(1) "y"
string(5) "given"
array(1) {
  [0]=>
  string(1) "d"
}
int(2)
Cannot modify readonly property R::$a
Cannot indirectly modify readonly property S::$b
bool(true)
bool(true)
int(5)
string(1) "y"
string(5) "given"
array(1) {
  [0]=>
  string(1) "d"
}
int(2)
Cannot modify readonly property R::$a
Cannot indirectly modify readonly property S::$b
bool(true)
