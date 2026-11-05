--TEST--
nocopy: frameless calls (PHP 8.4+) behave like the framed ones: errors name the function, strict_types, 1-arg apcu_store, namespaced fallback
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t040
apc.enable_cli=1
--FILE--
<?php
declare(strict_types=1);
namespace App;

var_dump(apcu_store(['a' => 1, 'b' => [2]]));       // arity 1: array form
var_dump(apcu_store('c', 3), apcu_store('d', 4, 60)); // arity 2, 3
var_dump(apcu_fetch('a'), apcu_fetch('b'), apcu_fetch('nope'), apcu_exists('c'), apcu_delete('c'), apcu_exists('c'));
var_dump(nocopy_put('e', 5), nocopy_put('f', 6, 60), nocopy_get('e'), nocopy_get('nope'));
var_dump(apcu_inc('n'), apcu_inc('n', 5), apcu_dec('n'), apcu_dec('n', 2));
try { apcu_store('x', 1, -1); } catch (\ValueError $e) { echo $e->getMessage(), "\n"; }
try { nocopy_put('x', 1, -1); } catch (\ValueError $e) { echo $e->getMessage(), "\n"; }
try { nocopy_get(1); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
try { nocopy_put('x', 1, '5'); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
try { nocopy_put('x', new \stdClass); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t040*')); ?>
--EXPECT--
array(0) {
}
bool(true)
bool(true)
int(1)
array(1) {
  [0]=>
  int(2)
}
bool(false)
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
int(5)
NULL
int(1)
int(6)
int(5)
int(3)
apcu_store(): Argument #3 ($ttl) must be greater than or equal to 0
nocopy_put(): Argument #3 ($ttl) must be greater than or equal to 0
nocopy_get(): Argument #1 ($key) must be of type string, int given
nocopy_put(): Argument #3 ($ttl) must be of type int, string given
nocopy_put(): Argument #2 ($value) must not contain objects or resources
