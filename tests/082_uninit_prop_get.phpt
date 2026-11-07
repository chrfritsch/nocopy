--TEST--
nocopy: a missing typed property comes back uninitialized (no __get), as from unserialize(), whether it was never set or unset()
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t082
--FILE--
<?php
class A {
    public int $x;
    public int $y;
    public function __get($n) { return 7; }
}
function probe($a, $p) { try { return var_export($a->$p, true); } catch (Error $e) { return $e->getMessage(); } }
$a = new A;
unset($a->y);
nocopy_put('a', $a);
foreach ([unserialize(serialize($a)), nocopy_get('a'), nocopy_get('a'), nocopy_get('a')] as $b) {
    echo probe($b, 'x'), ' / ', probe($b, 'y'), "\n";
}
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t082*')); ?>
--EXPECT--
Typed property A::$x must not be accessed before initialization / Typed property A::$y must not be accessed before initialization
Typed property A::$x must not be accessed before initialization / Typed property A::$y must not be accessed before initialization
Typed property A::$x must not be accessed before initialization / Typed property A::$y must not be accessed before initialization
Typed property A::$x must not be accessed before initialization / Typed property A::$y must not be accessed before initialization
