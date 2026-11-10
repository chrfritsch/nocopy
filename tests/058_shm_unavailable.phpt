--TEST--
nocopy.shm_name that cannot be attached (another user's): the cache is unavailable, never a private one; errors in the caller's values still throw
--EXTENSIONS--
nocopy
--SKIPIF--
<?php
if (!is_dir('/dev/shm')) die('skip needs /dev/shm');
if (!function_exists('posix_getuid') || posix_getuid() !== 0 || !is_executable('/usr/bin/setpriv')) die('skip needs root and setpriv');
?>
--INI--
nocopy.shm_name=/nocopy-t058
--FILE--
<?php
nocopy_put('mine', 'root');
$code = <<<'PHP'
var_dump(nocopy_shared_with());
var_dump(nocopy_put('a', 1), nocopy_get('a', $hit), $hit, nocopy_get('mine'));
var_dump(nocopy_put('b', [1]), nocopy_get('b'), nocopy_add('c', 1), nocopy_inc('n'), nocopy_cas('n', 0, 1), nocopy_delete('mine'));
var_dump(nocopy_put_multiple(['x' => 1]), nocopy_get_multiple(['x', 'mine']));
$calls = 0;
$f = function () use (&$calls) { return ++$calls; };
var_dump(nocopy_remember('r', $f), nocopy_remember('r', $f));
var_dump(nocopy_put('o', new stdClass));
var_dump(nocopy_stats()['size'], nocopy_stats()['shm_name'], nocopy_clear()); // off: a clear that reaches nothing fails
PHP;
echo shell_exec('/usr/bin/setpriv --reuid=nobody --regid=nogroup --clear-groups ' . escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
    . ' -d nocopy.shm_name=/nocopy-t058 -r ' . escapeshellarg($code) . ' 2>&1');
var_dump(nocopy_get('mine'), nocopy_get('a'), nocopy_get('b'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t058*')); ?>
--EXPECTF--
%AWarning: PHP Startup: nocopy: nocopy.shm_name /nocopy-t058: no permission to open it (another user created it). The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
string(4) "none"
bool(false)
NULL
bool(false)
NULL
bool(false)
NULL
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
array(0) {
}
int(1)
int(2)
bool(false)
int(0)
NULL
bool(false)
string(4) "root"
NULL
NULL
