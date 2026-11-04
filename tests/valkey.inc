<?php
// Minimal RESP2 client for the Valkey tests: another connection than nocopy's.
function vk(string ...$a)
{
    static $s;
    $s ??= fsockopen('127.0.0.1', 6379);
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

// A PHP process with its own nocopy memory: another web server on the same Valkey.
function other_server(string $prefix, string $code): string
{
    return (string) shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . " -d nocopy.valkey=127.0.0.1:6379 -d nocopy.valkey_prefix=$prefix -r " . escapeshellarg($code));
}
