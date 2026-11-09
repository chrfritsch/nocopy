--TEST--
nocopy: a class whose parent or interfaces changed is a miss, so a typed property never holds an object of the wrong class
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t089
--FILE--
<?php
require __DIR__ . '/serve.inc';
class Foo { public int $a = 1; }
interface I {}
class Bar extends Foo implements I {}
class Holder { public ?Foo $f = null; public ?I $i = null; }
$h = new Holder;
$h->f = new Bar;
nocopy_put('h', $h);
nocopy_put('b', new Bar);
// same name and slots, another parent: Holder::$f would hold a Bar that is no Foo
echo serve('<?php class Foo { public int $a = 1; } interface I {} class Bar implements I { public int $a = 1; } class Holder { public ?Foo $f = null; public ?I $i = null; }
var_dump(nocopy_get("h", $hit), $hit);');
// same parent, the interface gone
echo serve('<?php class Foo { public int $a = 1; } interface I {} class Bar extends Foo {} var_dump(nocopy_get("b", $hit), $hit);');
// unchanged: a hit
echo serve('<?php class Foo { public int $a = 1; } interface I {} class Bar extends Foo implements I {} class Holder { public ?Foo $f = null; public ?I $i = null; }
var_dump(nocopy_get("h")->f instanceof Foo);');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t089*')); ?>
--EXPECT--
NULL
bool(false)
NULL
bool(false)
bool(true)
