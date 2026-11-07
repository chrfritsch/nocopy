--TEST--
cli/nocopy: a separate process sees and changes this process's cache
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (!is_dir('/dev/shm')) die('skip needs /dev/shm'); ?>
--INI--
nocopy.shm_name=/nocopy-t085
--FILE--
<?php
function cli(string $args, string $name = '/nocopy-t085'): string
{
    exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . " -d nocopy.shm_name=$name " . escapeshellarg(__DIR__ . '/../cli/nocopy') . " $args 2>&1", $out, $rc);
    return implode("\n", $out) . " [$rc]\n";
}
nocopy_put('a.1', ['x' => 1], 60);
nocopy_put('a.2', 'two');
nocopy_put('b', 3);
echo cli('get a.1'), cli('get nope'), cli('keys a. --json');
echo cli('keys b');
var_dump(str_contains(cli('stats'), 'hit rate'), json_decode(substr(cli('stats --json'), 0, -5), true)['entries']);
echo cli('clear a.'), cli('delete b nope');
var_dump(nocopy_keys());
nocopy_put('c', 1);
echo cli('clear');
var_dump(nocopy_get('c'));
echo strtok(cli('frob'), "\n"), substr(cli('frob'), -6), cli('stats', "''");
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t085*')); ?>
--EXPECTF--
array(1) {
  ["x"]=>
  int(1)
} [0]
nocopy: no such key [1]
{
    "a.%d": {
        "size": %d,
        "created": %d,
        "expires": %d
    },
    "a.%d": {
        "size": %d,
        "created": %d,
        "expires": %d
    }
} [0]
key       size    age    ttl
b%w%d B%w%ds%w-
1 keys [0]
bool(true)
int(3)
2 deleted [0]
1 deleted [0]
array(0) {
}
cleared [0]
NULL
Usage: nocopy <command> [args] [--json] [64]
%Anocopy: no shared cache in this process%A [2]
