<?php
if (str_contains((string) @file_get_contents('/proc/self/maps'), 'asan')) die('skip libfaketime hangs under ASan'); // in SKIPIF
require_once __DIR__ . '/php.inc';
// runs $code in another PHP process whose wall clock is $off off (libfaketime); its monotonic clock is the host's
function faked(string $off, string $code, array $ini): string
{
    return trim(php_run($code, $ini, 'LD_PRELOAD=' . glob('/usr/lib/*/faketime/libfaketime.so.1')[0] . " FAKETIME=$off FAKETIME_DONT_FAKE_MONOTONIC=1 "));
}
