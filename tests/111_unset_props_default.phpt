--TEST--
nocopy: an unset declared property comes back with the class default, as unserialize(serialize()) gives it
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t111
--FILE--
<?php
class P { public $a = 1; public int $b = 5; public int $c; public $d = 'kept'; }
$p = new P;
unset($p->a, $p->b);
nocopy_put('k', $p);
$g = nocopy_get('k');
var_dump($g->a, $g->b, isset($g->c), $g->d, serialize($g) === serialize(unserialize(serialize($p))));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t111*')); ?>
--EXPECT--
int(1)
int(5)
bool(false)
string(4) "kept"
bool(true)
