--TEST--
nocopy: one "off" state: a process without a worker slot misses and fails writes and clears (no Error), says why in nocopy_stats(); settings an existing arena ignores are a warning
--EXTENSIONS--
nocopy
pcntl
posix
--INI--
nocopy.shm_name=/nocopy-t098
nocopy.shm_size=16M
--FILE--
<?php
require_once __DIR__ . '/php.inc';
nocopy_put('k', 'v');
var_dump(nocopy_stats()['off']);
$fds = [];
posix_setrlimit(POSIX_RLIMIT_NOFILE, 64, 64);
while (($f = @fopen('/dev/null', 'r')) !== false) $fds[] = $f; // no fd left: a forked child gets no liveness lock
$p = pcntl_fork();
if ($p === 0) {
    $fds = []; // fds for the output
    $s = nocopy_stats();
    echo "child: ", nocopy_shared_with(), ", ", str_starts_with((string) $s['off'], 'no worker slot') ? 'off: no worker slot' : var_export($s['off'], true), "\n";
    var_dump(nocopy_get('k'), nocopy_put('k2', 1), nocopy_delete('k'), nocopy_clear(), nocopy_inc('n'), nocopy_keys(), nocopy_remember('r', fn() => 'computed'));
    exit(0);
}
pcntl_waitpid($p, $st);
$fds = [];
echo "parent: ", nocopy_shared_with(), ", ", nocopy_get('k'), "\n";
// another process with other settings attaches to this arena: they are ignored, and it says so
echo php_run('echo nocopy_stats()["size"] >> 20, "M\n";', ['nocopy.shm_name' => '/nocopy-t098', 'nocopy.shm_size' => '64M']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t098*')); ?>
--EXPECTF--
NULL
%Achild: none, off: no worker slot
NULL
bool(false)
bool(false)
bool(false)
bool(false)
array(0) {
}
string(8) "computed"
parent: host, v
%AWarning: PHP Startup: nocopy: nocopy.shm_size or nocopy.entries_hint ignored: /nocopy-t098.%x was created with 16777216 bytes and other slot tables by a process still using it. Stop every process using it (CLI included) to apply them in Unknown on line 0
16M
