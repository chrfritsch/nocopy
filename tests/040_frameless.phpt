--TEST--
nocopy: frameless calls (PHP 8.4+) behave like the framed ones: errors name the function, strict_types, arities 1-3, namespaced fallback
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t040
--FILE--
<?php
declare(strict_types=1);
namespace App;

var_dump(nocopy_put('c', 3), nocopy_put('d', 4, 60)); // arity 2, 3
var_dump(nocopy_get('c'), nocopy_get('nope'));        // arity 1
try { nocopy_put('x', 1, -1); } catch (\ValueError $e) { echo $e->getMessage(), "\n"; }
try { nocopy_get(1); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
try { nocopy_put('x', 1, '5'); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
var_dump(nocopy_put('x', new \stdClass), nocopy_get('x') instanceof \stdClass);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t040*')); ?>
--EXPECT--
bool(true)
bool(true)
int(3)
NULL
nocopy_put(): Argument #3 ($ttl) must be greater than or equal to 0
nocopy_get(): Argument #1 ($key) must be of type string, int given
nocopy_put(): Argument #3 ($ttl) must be of type int, string given
bool(true)
bool(true)
