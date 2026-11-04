--TEST--
stash: frameless calls (PHP 8.4+) behave like the framed ones: errors name the function, strict_types, 1-arg apcu_store, namespaced fallback
--EXTENSIONS--
stash
--INI--
apc.enable_cli=1
--FILE--
<?php
declare(strict_types=1);
namespace App;

var_dump(apcu_store(['a' => 1, 'b' => [2]]));       // arity 1: array form
var_dump(apcu_store('c', 3), apcu_store('d', 4, 60)); // arity 2, 3
var_dump(apcu_fetch('a'), apcu_fetch('b'), apcu_fetch('nope'), apcu_exists('c'), apcu_delete('c'), apcu_exists('c'));
var_dump(stash_put('e', 5), stash_put('f', 6, 60), stash_get('e'), stash_get('nope'));
try { apcu_store('x', 1, -1); } catch (\ValueError $e) { echo $e->getMessage(), "\n"; }
try { stash_put('x', 1, -1); } catch (\ValueError $e) { echo $e->getMessage(), "\n"; }
try { stash_get(1); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
try { stash_put('x', 1, '5'); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
try { stash_put('x', new \stdClass); } catch (\TypeError $e) { echo $e->getMessage(), "\n"; }
?>
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
apcu_store(): Argument #3 ($ttl) must be greater than or equal to 0
stash_put(): Argument #3 ($ttl) must be greater than or equal to 0
stash_get(): Argument #1 ($key) must be of type string, int given
stash_put(): Argument #3 ($ttl) must be of type int, string given
stash_put(): Argument #2 ($value) must not contain objects or resources
