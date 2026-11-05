--TEST--
apcu_*: store/add/fetch/exists/delete, array forms, success flag
--EXTENSIONS--
nocopy
--INI--
apc.enable_cli=1
nocopy.shm_name=/nocopy-t008
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
var_dump(apcu_enabled());
var_dump(apcu_store('a', 1), apcu_fetch('a', $ok), $ok);
var_dump(apcu_fetch('nope', $ok), $ok);
var_dump(apcu_add('a', 2), apcu_add('b', 2), apcu_fetch('a'), apcu_fetch('b'));
var_dump(apcu_store('a', ['x' => [1, 2]]), apcu_fetch('a'));
var_dump(apcu_exists('a'), apcu_exists('nope'));

echo "-- arrays\n";
var_dump(apcu_store(['k1' => 'v1', 'k2' => 'v2', 5 => 'five']));
var_dump(apcu_fetch(['k1', 'k2', 'zz', '5'], $ok), $ok);
var_dump(apcu_exists(['k1', 'zz', 'k2']));
var_dump(apcu_add(['k1' => 'new', 'k9' => 'v9']));
var_dump(apcu_fetch('k1'), apcu_fetch('k9'));
var_dump(apcu_delete(['k1', 'zz', 'k2']));
var_dump(apcu_delete('k9'), apcu_delete('k9'));

echo "-- argument errors\n";
try { apcu_store('only-key'); } catch (ArgumentCountError $e) { echo get_class($e), "\n"; }
try { apcu_store('k', 1, -5); } catch (ValueError $e) { echo $e->getMessage(), "\n"; }

echo "-- clear\n";
var_dump(apcu_clear_cache(), apcu_fetch('a', $ok), $ok);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t008*')); ?>
--EXPECT--
bool(true)
bool(true)
int(1)
bool(true)
bool(false)
bool(false)
bool(false)
bool(true)
int(1)
int(2)
bool(true)
array(1) {
  ["x"]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
}
bool(true)
bool(false)
-- arrays
array(0) {
}
array(3) {
  ["k1"]=>
  string(2) "v1"
  ["k2"]=>
  string(2) "v2"
  [5]=>
  string(4) "five"
}
bool(true)
array(2) {
  ["k1"]=>
  bool(true)
  ["k2"]=>
  bool(true)
}
array(1) {
  ["k1"]=>
  int(-1)
}
string(2) "v1"
string(2) "v9"
array(1) {
  [0]=>
  string(2) "zz"
}
bool(true)
bool(false)
-- argument errors
ArgumentCountError
apcu_store(): Argument #3 ($ttl) must be greater than or equal to 0
-- clear
bool(true)
bool(false)
bool(false)
