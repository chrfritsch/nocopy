--TEST--
nocopy.shm_name created first by another user, or opened to others: root refuses to attach (it would read their pointers)
--EXTENSIONS--
nocopy
--SKIPIF--
<?php
if (!is_dir('/dev/shm')) die('skip needs /dev/shm');
if (!function_exists('posix_getuid') || posix_getuid() !== 0 || !is_executable('/usr/bin/setpriv')) die('skip needs root and setpriv');
?>
--INI--
nocopy.shm_name=/nocopy-t079-self
--FILE--
<?php
function run(string $code, bool $nobody = false): void
{
    $php = escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO')) . ' -d nocopy.shm_name=/nocopy-t079';
    echo shell_exec(($nobody ? '/usr/bin/setpriv --reuid=nobody --regid=nogroup --clear-groups ' : '') . $php . ' -r ' . escapeshellarg($code) . ' 2>&1');
}
$probe = 'var_dump(nocopy_shared_with(), nocopy_get("is_admin"));';

echo "nobody's, mode 0600:\n";
run('nocopy_put("is_admin", true);', true);
run($probe);

echo "nobody's, mode 0666:\n";
array_map(fn($f) => chmod($f, 0666), glob('/dev/shm/nocopy-t079.*'));
run($probe);

echo "root's, mode 0666:\n";
array_map('unlink', glob('/dev/shm/nocopy-t079.*'));
run('nocopy_put("is_admin", true);');
array_map(fn($f) => chmod($f, 0666), glob('/dev/shm/nocopy-t079.*'));
run($probe);

echo "root's, mode 0600:\n";
array_map(fn($f) => chmod($f, 0600), glob('/dev/shm/nocopy-t079.*'));
run($probe);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t079*')); ?>
--EXPECTF--
nobody's, mode 0600:
%AWarning: PHP Startup: nocopy: nocopy.shm_name /nocopy-t079: it is owned by another user or open to others. The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
string(4) "none"
NULL
nobody's, mode 0666:
%AWarning: PHP Startup: nocopy: nocopy.shm_name /nocopy-t079: it is owned by another user or open to others. The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
string(4) "none"
NULL
root's, mode 0666:
%AWarning: PHP Startup: nocopy: nocopy.shm_name /nocopy-t079: it is owned by another user or open to others. The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
string(4) "none"
NULL
root's, mode 0600:
string(4) "host"
bool(true)
