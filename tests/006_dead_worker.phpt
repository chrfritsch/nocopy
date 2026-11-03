--TEST--
stash: a worker that dies mid-request does not pin memory forever
--EXTENSIONS--
stash
pcntl
--INI--
apc.shm_size=4M
stash.request_scope=1
--FILE--
<?php
for ($n = 0; stash_put("v$n", range(1, 3000)) && stash_get("v0") !== null; $n++); // fill the arena until it evicts
$pid = pcntl_fork();
if ($pid === 0) {
    for ($i = 0; $i < $n; $i++) stash_get("v$i"); // child pins every segment...
    pcntl_exec('/bin/sh', ['-c', 'exit 0']); // ...and dies without RSHUTDOWN
}
pcntl_waitpid($pid, $st);
for ($i = 0; $i < $n; $i++) stash_delete("v$i");
$ok = 0;
for ($i = 0; $i < 400; $i++) {               // needs the dead child's pins gone
    $ok += stash_put('k', range(1, 1000));
}
var_dump($ok);
?>
--EXPECT--
int(400)
