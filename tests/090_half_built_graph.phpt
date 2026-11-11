--TEST--
nocopy: a graph that cannot be built (an enum case is gone) is a miss; the objects made before it are freed without reading their slots
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t090
--FILE--
<?php
require __DIR__ . '/serve.inc';
enum E { case X; case Y; }
class C { public $a; public $b; public $e; }
$c = new C;
$c->e = E::Y;
nocopy_put('c', $c);
nocopy_put('l', [$c, $c]);
// E::Y is gone: C is made first (its slots not yet filled), then the case is not found.
// The freed C of the same size left a string in the memory the new one gets: reading it as a slot releases $s a second time.
$reader = <<<'CODE'
<?php
enum E { case X; }
class C { public $a; public $b; public $e; }
foreach (['c', 'l'] as $k) {
    $s = str_repeat('x', 100);
    $o = new C;
    $o->a = $s;
    $o->b = $s;
    unset($o);
    var_dump(nocopy_get($k, $hit), $hit);
    $t = [str_repeat('y', 100), str_repeat('y', 100)];
    var_dump($s === str_repeat('x', 100));
}
CODE;
echo serve($reader);
file_put_contents($f = sys_get_temp_dir() . '/nocopy-t090.php', $reader);
passthru(php_cmd(['nocopy.shm_name' => '/nocopy-t090'], [$f]));
unlink($f);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t090*')); ?>
--EXPECT--
NULL
bool(false)
bool(true)
NULL
bool(false)
bool(true)
NULL
bool(false)
bool(true)
NULL
bool(false)
bool(true)
