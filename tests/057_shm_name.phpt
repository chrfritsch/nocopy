--TEST--
nocopy.shm_name: a separately started process (no fork) attaches to the same memory and reads zero-copy, empty arrays included
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (!is_dir('/dev/shm')) die('skip needs /dev/shm'); ?>
--INI--
apc.enable_cli=1
nocopy.request_scope=1
nocopy.shm_name=/nocopy-t057
--FILE--
<?php
function other(string $code, string $name = '/nocopy-t057'): string
{
    return (string) shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . " -d apc.enable_cli=1 -d nocopy.request_scope=1 -d nocopy.shm_name=$name -r " . escapeshellarg($code) . ' 2>&1');
}
apcu_clear_cache();
nocopy_put('k', ['s' => str_repeat('x', 40), 'e' => [], 'n' => [[], ['a' => []]], 'i' => 7]);
echo other('$v = nocopy_get("k"); $e = $v["e"]; $e[] = 1; echo json_encode($v), " ", json_encode($e), "\n"; nocopy_put("from_b", [1, []]);');
var_dump(nocopy_get('from_b'));
echo other('var_dump(nocopy_get("k"));', '/nocopy-t057-other');
echo count(glob('/dev/shm/nocopy-t057.*')), "\n";
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t057*')); ?>
--EXPECT--
{"s":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","e":[],"n":[[],{"a":[]}],"i":7} [1]
array(2) {
  [0]=>
  int(1)
  [1]=>
  array(0) {
  }
}
NULL
1
