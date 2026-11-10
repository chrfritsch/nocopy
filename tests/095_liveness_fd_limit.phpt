--TEST--
nocopy: a process that cannot open /proc (at its fd limit) must not take a live lease holder for dead (design review 2026-11, finding 1)
--EXTENSIONS--
nocopy
pcntl
posix
--INI--
nocopy.shm_name=/nocopy-t095
--XFAIL--
Liveness fails open: ar_owner_dead() reads "cannot open /proc/<pid>/stat" as dead (arena/proc.c). The same answer steals shard locks and worker slots.
--FILE--
<?php
nocopy_put('calls', 0);
$holder = pcntl_fork();
if ($holder === 0) {
    nocopy_remember('k', function () { nocopy_inc('calls'); usleep(800000); return 'holder'; }, 60);
    exit(0);
}
usleep(200000); // the holder has the lease and is computing
$second = pcntl_fork();
if ($second === 0) {
    posix_setrlimit(POSIX_RLIMIT_NOFILE, 64, 64);
    $fds = [];
    while (($f = @fopen('/dev/null', 'r')) !== false) $fds[] = $f; // no fd left for /proc/<pid>/stat
    $v = nocopy_remember('k', function () { nocopy_inc('calls'); return 'second'; }, 60);
    exit($v === 'holder' ? 0 : 1);
}
pcntl_waitpid($second, $st);
pcntl_waitpid($holder, $st2);
echo "waiter got the holder's value: ", pcntl_wexitstatus($st) === 0 ? 'yes' : 'no', "\n";
echo nocopy_get('calls'), " call\n";
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t095*')); ?>
--EXPECT--
waiter got the holder's value: yes
1 call
