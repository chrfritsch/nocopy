--TEST--
nocopy: a process running out of memory in apcu_cache_info() does not leave the arena locked
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t013
apc.enable_cli=1
apc.shm_size=32M
--FILE--
<?php
for ($i = 0; $i < 60000; $i++) apcu_store("key$i", $i);
$pid = pcntl_fork();
if ($pid === 0) {
    ini_set('memory_limit', '8M'); // bails out (PHP <= 8.6) or _exit()s at the hard limit (8.7+)
    apcu_cache_info();
    exit(0);
}
pcntl_waitpid($pid, $st);
pcntl_alarm(5); // a stuck lock spins forever: die instead
var_dump(apcu_store('after', 1));
var_dump(count(apcu_cache_info()['cache_list']));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t013*')); ?>
--EXPECTF--
%Abool(true)
int(60001)
