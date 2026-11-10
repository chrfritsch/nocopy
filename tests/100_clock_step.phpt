--TEST--
nocopy: TTLs run on the monotonic clock: a writer whose wall clock is an hour off neither expires entries early nor keeps them late (design review 2026-11, finding 6)
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require_once __DIR__ . '/clock_step.inc'; if (!glob('/usr/lib/*/faketime/libfaketime.so.1') || !getenv('NOCOPY_SO')) die('skip libfaketime or NOCOPY_SO missing'); ?>
--INI--
nocopy.shm_name=/nocopy-t100
--FILE--
<?php
require __DIR__ . '/clock_step.inc';
$ini = '-d nocopy.shm_name=/nocopy-t100';
echo faked('-1h', 'nocopy_put("behind", "v", 100); echo time() < ' . time() . ' - 3000 ? "behind" : "not faked";', $ini), "\n";
echo faked('+1h', 'nocopy_put("ahead", "v", 1); echo time() > ' . time() . ' + 3000 ? "ahead" : "not faked";', $ini), "\n";
var_dump(nocopy_get('behind'), nocopy_get('ahead'));
$k = nocopy_keys()['behind']; // wall time, in this process's clock
var_dump(abs($k['created'] - time()) <= 2, $k['expires'] - $k['created']);
sleep(3);
var_dump(nocopy_get('behind'), nocopy_get('ahead'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t100*')); ?>
--EXPECT--
behind
ahead
string(1) "v"
string(1) "v"
bool(true)
int(100)
string(1) "v"
NULL
