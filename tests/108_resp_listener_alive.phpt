--TEST--
nocopy.resp: the listener keeps its liveness lock (an OFD lock on the arena), so workers do not take its shard locks as a dead holder's
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/resp_skip.inc'; if (PHP_OS_FAMILY !== 'Linux') die('skip OFD locks: Linux'); ?>
--INI--
nocopy.shm_name=/nocopy-t108
nocopy.resp=127.0.0.1:6379
nocopy.resp_prefix=t108:
--CONFLICTS--
resp
--FILE--
<?php
require __DIR__ . '/resp.inc';
nocopy_put('a', 1);
settle();
$locks = 0;
foreach (glob('/proc/[0-9]*/comm') as $comm) {
    if (trim((string) @file_get_contents($comm)) !== 'nocopy-resp') {
        continue;
    }
    foreach (glob(dirname($comm) . '/fdinfo/*') as $fd) {
        $locks += substr_count((string) @file_get_contents($fd), 'OFDLCK');
    }
}
var_dump($locks > 0);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t108*')); ?>
--EXPECT--
bool(true)
