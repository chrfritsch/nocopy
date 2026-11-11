<?php
require_once __DIR__ . '/php.inc';
// Runs $code as one request under php -S (short-lived SAPI: zero-copy) with this test's nocopy.* INI plus $ini; returns the response.
// An array of codes: one request each, in order, on the same server process; the responses joined.
function serve(string|array $code, array $ini = []): string
{
    $dir = sys_get_temp_dir() . '/nocopy-serve-' . getmypid() . '-' . mt_rand();
    mkdir($dir);
    $codes = (array) $code;
    foreach ($codes as $i => $c) {
        file_put_contents("$dir/$i.php", $c);
    }
    $s = stream_socket_server('tcp://127.0.0.1:0');
    $addr = stream_socket_get_name($s, false);
    fclose($s);
    $cmd = php_argv(['max_execution_time' => 0] + ini_get_all('nocopy', false) + $ini, ['-S', $addr, '-t', $dir]);
    $p = proc_open($cmd, [1 => ['file', '/dev/null', 'w'], 2 => ['file', "$dir/err", 'w']], $pipes);
    $ctx = stream_context_create(['http' => ['ignore_errors' => true, 'timeout' => 3600]]);
    for ($i = 0; $i < 500 && ($out = @file_get_contents("http://$addr/0.php", false, $ctx)) === false; $i++) {
        usleep(10000);
    }
    for ($i = 1; $i < count($codes) && $out !== false; $i++) {
        $out .= file_get_contents("http://$addr/$i.php", false, $ctx);
    }
    usleep(20000);
    $st = proc_get_status($p);
    proc_terminate($p);
    proc_close($p);
    // crashes after the response was sent (request shutdown) would pass unseen
    if (!$st['running']) {
        $out .= "\nphp -S died: " . ($st['signaled'] ? "signal $st[termsig]" : "exit $st[exitcode]") . "\n";
    }
    $err = file_get_contents("$dir/err");
    if (preg_match('/Sanitizer|runtime error|memory leaks detected/', $err)) {
        $out .= "\n$err";
    }
    unlink("$dir/err");
    array_map('unlink', glob("$dir/*.php"));
    rmdir($dir);
    return (string) $out;
}
