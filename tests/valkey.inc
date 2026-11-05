<?php
// Minimal RESP2 client for the Valkey tests: another connection than nocopy's.
function vk(string ...$a)
{
    static $s;
    $s ??= fsockopen('127.0.0.1', $GLOBALS['VK_PORT'] ?? 6379);
    $c = '*' . count($a) . "\r\n";
    foreach ($a as $x) {
        $c .= '$' . strlen($x) . "\r\n$x\r\n";
    }
    fwrite($s, $c);
    $l = fgets($s);
    if ($l[0] === ':') {
        return (int) substr($l, 1);
    }
    if ($l[0] === '$') {
        $n = (int) substr($l, 1);
        if ($n < 0) {
            return null;
        }
        $d = '';
        while (strlen($d) < $n + 2) {
            $d .= fread($s, $n + 2 - strlen($d));
        }
        return substr($d, 0, $n);
    }
    return rtrim(substr($l, 1));
}

// Returns once nocopy's listener has applied every change made in Valkey before the call: a local copy of a marker
// is dropped only by an invalidation for it, and those arrive in order.
function settle(): void
{
    static $n = 0;
    $k = ini_get('nocopy.valkey_prefix') . '__settle';
    vk('SET', $k, (string) ++$n);
    for ($i = 0; $i < 2000; $i++, usleep(1000)) {
        $h = nocopy_stats()['hits'];
        if (nocopy_get('__settle') === $n && nocopy_stats()['hits'] > $h) {
            break; // held locally
        }
    }
    vk('SET', $k, (string) ++$n);
    for ($i = 0; $i < 2000 && nocopy_get('__settle') !== $n; $i++) {
        usleep(1000);
    }
}

// A PHP process with its own nocopy memory: another web server on the same Valkey.
function other_server(string $prefix, string $code): string
{
    return (string) shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . " -d nocopy.valkey=127.0.0.1:6379 -d nocopy.valkey_prefix=$prefix -r " . escapeshellarg($code));
}
