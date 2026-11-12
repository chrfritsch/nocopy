--TEST--
nocopy.resp: a TTL goes to RESP as PX: a writer whose wall clock is an hour behind still stores for the TTL, and another host reads it (design review 2026-11, finding 6)
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require_once __DIR__ . '/clock_step.inc'; if (!glob('/usr/lib/*/faketime/libfaketime.so.1')) die('skip libfaketime missing'); require __DIR__ . '/resp_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t101
--CONFLICTS--
resp
--FILE--
<?php
require __DIR__ . '/resp.inc';
require __DIR__ . '/clock_step.inc';
$vk = fn($host) => ['nocopy.shm_name' => "/nocopy-t101$host", 'nocopy.resp' => '127.0.0.1:6379', 'nocopy.resp_prefix' => 't101:'];
vk('DEL', 't101:v');
faked('-1h', 'nocopy_put("v", "from behind", 100);', $vk('w'));
var_dump(vk('PTTL', 't101:v') > 90000);
echo faked('+1h', 'echo nocopy_get("v") ?? "miss";', $vk('r')), "\n"; // a reader an hour ahead: RESP decides
vk('DEL', 't101:v');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t101*')); ?>
--EXPECT--
bool(true)
from behind
