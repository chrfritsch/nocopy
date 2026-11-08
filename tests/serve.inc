<?php
// Runs $code as one request under php -S (short-lived SAPI: zero-copy) with this test's nocopy.* INI plus $ini; returns the response.
function serve(string $code, array $ini = []): string
{
    $dir = sys_get_temp_dir() . '/nocopy-serve-' . getmypid() . '-' . mt_rand();
    mkdir($dir);
    file_put_contents("$dir/index.php", $code);
    $s = stream_socket_server('tcp://127.0.0.1:0');
    $addr = stream_socket_get_name($s, false);
    fclose($s);
    $cmd = [PHP_BINARY, '-n', '-d', 'extension=' . getenv('NOCOPY_SO'), '-d', 'max_execution_time=0'];
    foreach (ini_get_all('nocopy', false) + $ini as $k => $v) {
        array_push($cmd, '-d', "$k=$v");
    }
    array_push($cmd, '-S', $addr, '-t', $dir);
    $p = proc_open($cmd, [1 => ['file', '/dev/null', 'w'], 2 => ['file', "$dir/err", 'w']], $pipes);
    $ctx = stream_context_create(['http' => ['ignore_errors' => true, 'timeout' => 3600]]);
    for ($i = 0; $i < 500 && ($out = @file_get_contents("http://$addr/", false, $ctx)) === false; $i++) {
        usleep(10000);
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
    if (preg_match('/Sanitizer|runtime error/', $err)) {
        $out .= "\n$err";
    }
    unlink("$dir/err");
    unlink("$dir/index.php");
    rmdir($dir);
    return (string) $out;
}
