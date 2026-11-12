--TEST--
nocopy.resp: an integer read from RESP keeps its TTL locally (its payload carries none), single and multi reads
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/resp_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t105
nocopy.resp=127.0.0.1:6379
nocopy.resp_prefix=t105:
--CONFLICTS--
resp
--FILE--
<?php
require __DIR__ . '/resp.inc';
nocopy_clear();
function exp_in(string $k): ?int
{
    $e = nocopy_keys($k)[$k]['expires'] ?? null;
    return $e ? $e - time() : $e;
}
var_dump(nocopy_inc('c', 1, 100), nocopy_inc('m', 1, 100), nocopy_inc('never'));
settle(); // our own INCRs' invalidations, else one landing during the read drops the copy
var_dump(nocopy_get('c'));
var_dump(exp_in('c') >= 97 && exp_in('c') <= 100);
var_dump(nocopy_get_multiple(['m', 'never']));
var_dump(exp_in('m') >= 97 && exp_in('m') <= 100, exp_in('never'));
nocopy_inc('short', 1, 1);
var_dump(nocopy_get('short'));
usleep(1100000);
var_dump(nocopy_get('short'));
nocopy_clear();
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t105*')); ?>
--EXPECT--
int(1)
int(1)
int(1)
int(1)
bool(true)
array(2) {
  ["m"]=>
  int(1)
  ["never"]=>
  int(1)
}
bool(true)
int(0)
int(1)
NULL
