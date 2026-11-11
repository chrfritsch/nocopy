--TEST--
nocopy.valkey: the listener holds nothing of the process that started it open, and exits once its arena is removed
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; if (!is_dir('/proc/self')) die('skip needs /proc'); ?>
--INI--
nocopy.shm_name=/nocopy-t062-parent
--CONFLICTS--
valkey
--FILE--
<?php
$code = '$f = fopen(PHP_BINARY, "r"); $s = stream_socket_server("tcp://127.0.0.1:0"); nocopy_put("a", 1); echo nocopy_stats()["valkey_listener"], "\n"; fgets(STDIN);';
$p = proc_open([PHP_BINARY, '-n', '-d', 'extension=' . getenv('NOCOPY_SO'), '-d', 'nocopy.shm_name=/nocopy-t062', '-d', 'nocopy.valkey=127.0.0.1:6379', '-d', 'nocopy.valkey_prefix=t062:', '-r', $code],
    [0 => ['pipe', 'r'], 1 => ['pipe', 'w']], $pipes);
$pid = (int) fgets($pipes[1]);
$fds = array_map(fn ($f) => @readlink("/proc/$pid/fd/$f"), array_diff(scandir("/proc/$pid/fd"), ['.', '..']));
var_dump($pid > 0, count($fds), count(preg_grep('/^socket:/', $fds)), count(preg_grep('#^/dev/null$#', $fds)));
fclose($pipes[0]);
proc_close($p);
usleep(300000);
var_dump(file_exists("/proc/$pid"));
array_map('unlink', glob('/dev/shm/nocopy-t062.*'));
usleep(300000);
$st = @file_get_contents("/proc/$pid/stat");
var_dump($st === false || (bool) preg_match('/^\d+ \(.*\) Z/', $st));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t062*')); ?>
--EXPECT--
bool(true)
int(5)
int(1)
int(3)
bool(true)
bool(true)
