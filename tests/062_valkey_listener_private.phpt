--TEST--
nocopy.valkey without nocopy.shm_name: the listener of a process's own arena holds nothing of it open and exits with its last user
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; if (!is_dir('/proc/self')) die('skip needs /proc'); ?>
--FILE--
<?php
$code = '$f = fopen(PHP_BINARY, "r"); $s = stream_socket_server("tcp://127.0.0.1:0"); nocopy_put("a", 1); echo nocopy_stats()["valkey_listener"], "\n"; fgets(STDIN);';
$p = proc_open([PHP_BINARY, '-n', '-d', 'extension=' . getenv('NOCOPY_SO'), '-d', 'apc.enable_cli=1', '-d', 'nocopy.valkey=127.0.0.1:6379', '-d', 'nocopy.valkey_prefix=t062:', '-r', $code],
    [0 => ['pipe', 'r'], 1 => ['pipe', 'w']], $pipes);
$pid = (int) fgets($pipes[1]);
$fds = array_map(fn ($f) => @readlink("/proc/$pid/fd/$f"), array_diff(scandir("/proc/$pid/fd"), ['.', '..']));
var_dump($pid > 0, count($fds), count(preg_grep('/^socket:/', $fds)), count(preg_grep('/^pipe:/', $fds)), count(preg_grep('#^/dev/null$#', $fds)));
fclose($pipes[0]);
proc_close($p);
usleep(300000);
$st = @file_get_contents("/proc/$pid/stat");
var_dump($st === false || (bool) preg_match('/^\d+ \(.*\) Z/', $st));
?>
--EXPECT--
bool(true)
int(5)
int(1)
int(1)
int(3)
bool(true)
