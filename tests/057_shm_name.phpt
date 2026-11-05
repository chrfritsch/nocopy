--TEST--
nocopy.shm_name: a separately started process (no fork) attaches to the same memory and reads zero-copy, empty arrays included
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (!is_dir('/dev/shm')) die('skip needs /dev/shm'); ?>
--INI--
nocopy.request_scope=1
nocopy.shm_name=/nocopy-t057
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
function other(string $code, string $name = '/nocopy-t057'): string
{
    return (string) shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . " -d nocopy.request_scope=1 -d nocopy.shm_name=$name -r " . escapeshellarg($code) . ' 2>&1');
}
apcu_clear_cache();
nocopy_put('k', ['s' => str_repeat('x', 40), 'e' => [], 'n' => [[], ['a' => []]], 'i' => 7]);
echo other('$v = nocopy_get("k"); $e = $v["e"]; $e[] = 1; echo json_encode($v), " ", json_encode($e), "\n"; nocopy_put("from_b", [1, []]);');
var_dump(nocopy_get('from_b'));
echo other('var_dump(nocopy_get("k"));', '/nocopy-t057-other');
echo count(glob('/dev/shm/nocopy-t057.*')), " ", (int) str_starts_with(nocopy_stats()['shm_name'], '/nocopy-t057.'), " ", nocopy_shared_with(), "\n";
echo other('var_dump(nocopy_shared_with(), nocopy_stats()["shm_name"]);', "''");
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t057*')); ?>
--EXPECTF--
{"s":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","e":[],"n":[[],{"a":[]}],"i":7} [1]
array(2) {
  [0]=>
  int(1)
  [1]=>
  array(0) {
  }
}
NULL
1 1 host
%AWarning: PHP Startup: nocopy: nocopy.shm_name is not set (e.g. /nocopy). The cache is unavailable in this process: fetches miss, writes fail in Unknown on line 0
string(4) "none"
NULL
