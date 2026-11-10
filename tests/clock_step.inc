<?php
if (str_contains((string) @file_get_contents('/proc/self/maps'), 'asan')) die('skip libfaketime hangs under ASan'); // in SKIPIF
// runs $code in another PHP process whose wall clock is $off off (libfaketime); its monotonic clock is the host's
function faked(string $off, string $code, string $ini): string
{
    return trim((string) shell_exec('LD_PRELOAD=' . glob('/usr/lib/*/faketime/libfaketime.so.1')[0]
        . " FAKETIME=$off FAKETIME_DONT_FAKE_MONOTONIC=1 " . escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . " $ini -r " . escapeshellarg($code) . ' 2>&1'));
}
