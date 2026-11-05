--TEST--
nocopy: writers killed mid-write leave nothing behind: no stuck segments, no live bytes after a clear, misses never hang
--EXTENSIONS--
nocopy
pcntl
--INI--
nocopy.shm_name=/nocopy-t037
apc.enable_cli=1
apc.shm_size=16M
--FILE--
<?php
$small = str_repeat("x", 1500);   // filled under the shard lock
$big = str_repeat("y", 100000);   // a run of segments, filled outside the lock
for ($round = 0; $round < 40; $round++) {
    $pid = pcntl_fork();
    if ($pid === 0) {
        for ($i = 0; ; $i++) {
            $k = "a" . ($i % 3000);
            match ($i % 4) { 0, 1 => nocopy_put($k, $small), 2 => nocopy_put("big" . ($i % 7), $big), 3 => nocopy_delete($k) };
        }
    }
    usleep(mt_rand(2000, 20000));
    exec("kill -9 $pid");
    pcntl_alarm(5); // a hang (lock or a miss retried forever) kills this process instead
    for ($i = 0; $i < 2000; $i++) { nocopy_put("b$i", $small); nocopy_get("missing$i"); }
    pcntl_alarm(0);
    pcntl_waitpid($pid, $st);
}
apcu_clear_cache();
nocopy_put('z', 1); nocopy_delete('z');
$s = nocopy_stats();
$open = 7 * $s['shards']; // at most one open segment per TTL bucket and shard
$ok = $s["segments"] - $s["free_segments"] - $s["dead_segments"] <= $open;
var_dump($s["used"], $s["entries"], $ok);
if (!$ok) print_r($s);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t037*')); ?>
--EXPECT--
int(0)
int(0)
bool(true)
