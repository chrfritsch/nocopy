--TEST--
nocopy: properties __sleep() leaves out get the evaluated class default (an enum case, a constant) also under opcache
--EXTENSIONS--
nocopy
opcache
--INI--
nocopy.shm_name=/nocopy-t109
opcache.enable=1
opcache.enable_cli=1
--FILE--
<?php
enum S { case Draft; }
define('T', 'tee');
class Doc { public S $s = S::Draft; public $c = T; public $t = 'x'; function __sleep() { return ['t']; } }
nocopy_put('d', new Doc);
$d = nocopy_get('d');
var_dump($d->s === S::Draft, $d->c, $d->t, serialize($d) === serialize(unserialize(serialize(new Doc))));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t109*')); ?>
--EXPECT--
bool(true)
string(3) "tee"
string(1) "x"
bool(true)
