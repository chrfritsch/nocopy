--TEST--
nocopy.valkey: an idle connection survives Valkey's `timeout` (subscribed clients are exempt) and keeps receiving invalidations
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t056
apc.enable_cli=1
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t056:
--FILE--
<?php
require __DIR__ . '/valkey.inc';
// own short-lived connections: the timeout closes idle ones, valkey.inc's included
function cmd(string ...$a) {
    $s = fsockopen('127.0.0.1', 6379);
    $c = '*' . count($a) . "\r\n";
    foreach ($a as $x) {
        $c .= '$' . strlen($x) . "\r\n$x\r\n";
    }
    fwrite($s, $c);
    $r = fgets($s);
    fclose($s);
    return rtrim($r);
}
nocopy_put('k', 'old');
var_dump(cmd('CONFIG', 'SET', 'timeout', '1'));
sleep(3);
var_dump(cmd('SET', 't056:k', '7'));
settle();
var_dump(nocopy_get('k'));
var_dump(cmd('CONFIG', 'SET', 'timeout', '0'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t056*')); ?>
--EXPECT--
string(3) "+OK"
string(3) "+OK"
int(7)
string(3) "+OK"
