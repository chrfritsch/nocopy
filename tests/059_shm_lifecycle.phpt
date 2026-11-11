--TEST--
nocopy.shm_name lifecycle: another build's unused arena is removed, one in use is reported and kept, a creator's unfinished arena is replaced
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (!is_dir('/dev/shm')) die('skip needs /dev/shm'); ?>
--INI--
nocopy.shm_name=/nocopy-t059-parent
--FILE--
<?php
require_once __DIR__ . '/php.inc';
function child(string $base, string $code): string
{
    return php_run($code, ['nocopy.shm_name' => $base]);
}
// this build's suffix (it does not depend on the base)
$probe = trim(child('/nocopy-t059p', 'echo nocopy_stats()["shm_name"];'));
unlink('/dev/shm/' . substr($probe, 1));
$ours = '/dev/shm/nocopy-t059' . substr($probe, strlen('/nocopy-t059p'));

// another build's, unused: removed
touch('/dev/shm/nocopy-t059.deadbeef');
chmod('/dev/shm/nocopy-t059.deadbeef', 0600);
// another build's, in use (its processes hold a shared flock): reported, kept
$busy = fopen('/dev/shm/nocopy-t059.cafebabe', 'c');
chmod('/dev/shm/nocopy-t059.cafebabe', 0600);
flock($busy, LOCK_SH);
// ours, left empty by a creator that died: replaced
touch($ours);
chmod($ours, 0600);

echo child('/nocopy-t059', 'echo nocopy_shared_with(), " ", nocopy_stats()["size"] > 0 ? "sized" : "empty", "\n";');
var_dump(file_exists('/dev/shm/nocopy-t059.deadbeef'), file_exists('/dev/shm/nocopy-t059.cafebabe'), filesize($ours) > 0);

// once its processes are gone, the next start removes it
flock($busy, LOCK_UN);
fclose($busy);
echo child('/nocopy-t059', 'echo nocopy_shared_with(), "\n";');
var_dump(file_exists('/dev/shm/nocopy-t059.cafebabe'));
echo child('nocopy-t059', ''), child('/a/b', '');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t059*')); ?>
--EXPECTF--
%AWarning: PHP Startup: nocopy: /nocopy-t059.cafebabe, the nocopy.shm_name cache of another PHP or nocopy build, is in use: its processes do not share this cache in Unknown on line 0
host sized
bool(false)
bool(true)
bool(true)
host
bool(false)
%AWarning: PHP Startup: nocopy: nocopy.shm_name nocopy-t059: not a name like /nocopy (one leading slash, at most 40 characters). The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
%AWarning: PHP Startup: nocopy: nocopy.shm_name /a/b: not a name like /nocopy (one leading slash, at most 40 characters). The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
