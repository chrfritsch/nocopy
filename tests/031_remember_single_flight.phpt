--TEST--
stash: concurrent misses run the callback once (stash_remember, apcu_entry); a killed lease holder does not block the others
--EXTENSIONS--
stash
pcntl
--INI--
apc.enable_cli=1
--FILE--
<?php
foreach (['remember' => fn($cb) => stash_remember('k1', $cb, 60), 'apcu_entry' => fn($cb) => apcu_entry('k2', $cb, 60)] as $name => $get) {
    apcu_store('calls', 0);
    $pids = [];
    for ($i = 0; $i < 8; $i++) {
        $p = pcntl_fork();
        if ($p === 0) {
            $v = $get(function () { apcu_inc('calls'); usleep(300000); return 'val'; });
            exit($v === 'val' ? 0 : 1);
        }
        $pids[] = $p;
    }
    $ok = 0;
    foreach ($pids as $p) { pcntl_waitpid($p, $st); $ok += pcntl_wexitstatus($st) === 0; }
    echo "$name: $ok ok, ", apcu_fetch('calls'), " call\n";
}

$p = pcntl_fork();
if ($p === 0) { stash_remember('d', function () { sleep(60); }); exit; }
usleep(200000);
exec("kill -9 $p"); // a zombie until reaped below
$t = microtime(true);
var_dump(stash_remember('d', fn() => 'mine'), microtime(true) - $t < 2);
pcntl_waitpid($p, $st);
?>
--EXPECT--
remember: 8 ok, 1 call
apcu_entry: 8 ok, 1 call
string(4) "mine"
bool(true)
