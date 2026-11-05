--TEST--
nocopy: a worker that dies mid-request does not pin memory forever
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t006
nocopy.shm_size=4M
--FILE--
<?php
require __DIR__ . '/serve.inc';
echo serve(<<<'CODE'
<?php
for ($n = 0; nocopy_put("v$n", range(1, 3000)) && nocopy_stats()['expunges'] == 0; $n++); // fill the arena until it evicts
$pid = pcntl_fork();
if ($pid === 0) {
    for ($i = 0; $i < $n; $i++) nocopy_get("v$i"); // child pins every segment...
    pcntl_exec('/bin/sh', ['-c', 'exit 0']); // ...and dies without RSHUTDOWN
}
pcntl_waitpid($pid, $st);
for ($i = 0; $i < $n; $i++) nocopy_delete("v$i");
$ok = 0;
for ($i = 0; $i < 400; $i++) {               // needs the dead child's pins gone
    $ok += nocopy_put('k', range(1, 1000));
}
var_dump($ok);
CODE, ['extension' => 'pcntl']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t006*')); ?>
--EXPECT--
int(400)
