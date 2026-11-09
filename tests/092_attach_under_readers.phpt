--TEST--
nocopy: processes attaching to the arena never show its readers anything of their own (a cached empty array read zero-copy while others start)
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t092
--FILE--
<?php
require __DIR__ . '/serve.inc';
nocopy_put('e', []);
nocopy_put('n', ['a' => [], 'b' => 1]);
$secs = 4;
if (!pcntl_fork()) { // other binaries' instances (their own ASLR) attach while the request below reads
    $cmd = [PHP_BINARY, '-n', '-d', 'extension=' . getenv('NOCOPY_SO'), '-d', 'nocopy.shm_name=/nocopy-t092', '-r', ''];
    for ($end = microtime(true) + $secs + 0.5; microtime(true) < $end;) {
        $ps = [];
        for ($i = 0; $i < 3; $i++) $ps[] = proc_open($cmd, [], $p);
        array_map('proc_close', $ps);
    }
    exit(0);
}
echo serve('<?php $e = nocopy_get("e"); $n = nocopy_get("n"); $bad = 0;
for ($end = microtime(true) + ' . $secs . '; microtime(true) < $end;) for ($j = 0; $j < 100000; $j++) $bad += isset($e["k"]) + isset($n["a"]["k"]);
var_dump($bad, $e, $n);');
pcntl_wait($st);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t092*')); ?>
--EXPECT--
int(0)
array(0) {
}
array(2) {
  ["a"]=>
  array(0) {
  }
  ["b"]=>
  int(1)
}
