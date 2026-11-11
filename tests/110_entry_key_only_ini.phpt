--TEST--
APCu polyfill: apcu_entry() passes its callback the key alone (as APCu); a negative nocopy.local_size is refused
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t110
nocopy.local_size=-1
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
apcu_clear_cache();
var_dump(apcu_entry('a', fn (string $k, ?array $opts = null) => [$k, $opts]));
var_dump(apcu_entry('b', function ($k, &$meta = null) { $meta = [1]; return func_num_args(); }, 100));
var_dump(ini_get('nocopy.local_size'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t110*')); ?>
--EXPECTF--
Warning: PHP Startup: nocopy: nocopy.local_size must not be negative, using the default in %s
array(2) {
  [0]=>
  string(1) "a"
  [1]=>
  NULL
}
int(1)
string(2) "8M"
