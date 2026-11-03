--TEST--
stash: a writer killed inside a lock does not deadlock the others (even while it is an unreaped zombie)
--EXTENSIONS--
stash
pcntl
--INI--
apc.shm_size=16M
--FILE--
<?php
$v = str_repeat("x", 1500); // inline put: the copy runs under the shard lock
for ($round = 0; $round < 30; $round++) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        for ($i = 0; ; $i++) stash_put("a" . ($i % 5000), $v);
    }
    usleep(mt_rand(2000, 20000));
    exec("kill -9 $pid");
    pcntl_alarm(5); // a deadlock kills this process instead of hanging the run
    for ($i = 0; $i < 2000; $i++) stash_put("b$i", $v); // every shard, before reaping: the holder may be a zombie
    pcntl_alarm(0);
    pcntl_waitpid($pid, $st);
}
var_dump(stash_get("b1999") === $v, stash_stats()['lock_steals'] > 0);
?>
--EXPECT--
bool(true)
bool(true)
