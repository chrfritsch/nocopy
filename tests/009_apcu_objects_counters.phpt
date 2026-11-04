--TEST--
apcu_*: objects via serialization, inc/dec/cas, entry, cache_info, atomic counters across workers
--EXTENSIONS--
nocopy
pcntl
--INI--
apc.enable_cli=1
--FILE--
<?php
class P { public $a = 1; public $list = []; public ?P $child = null; }
$p = new P; $p->list = [1, 'two']; $p->child = new P;
apcu_store('obj', $p);
$q = apcu_fetch('obj');
var_dump($q == $p, $q !== $p, $q->child instanceof P);
apcu_store('mixed', ['plain' => [1, 2], 'o' => new stdClass]);
var_dump(apcu_fetch('mixed')['o'] instanceof stdClass, apcu_fetch('mixed')['plain']);
try { apcu_store('cl', fn() => 1); } catch (Exception $e) { echo get_class($e), "\n"; }

echo "-- counters\n";
var_dump(apcu_inc('n', 1, $ok), $ok, apcu_inc('n', 5), apcu_dec('n', 2), apcu_fetch('n'));
apcu_store('s', 'str');
var_dump(apcu_inc('s', 1, $ok), $ok);
var_dump(apcu_cas('n', 4, 10), apcu_cas('n', 4, 11), apcu_fetch('n'), apcu_cas('s', 1, 2));

echo "-- entry\n";
$calls = 0;
$gen = function ($key) use (&$calls) { $calls++; return "made-$key"; };
var_dump(apcu_entry('e', $gen), apcu_entry('e', $gen), $calls);

echo "-- info\n";
$i = apcu_cache_info();
var_dump(isset($i['num_hits'], $i['num_misses'], $i['cache_list'], $i['num_slots']), $i['num_entries'] > 0);
var_dump(array_key_exists('cache_list', apcu_cache_info(true)));
$m = apcu_sma_info();
var_dump($m['num_seg'] === nocopy_stats()['shards'], $m['avail_mem'] > 0);

echo "-- atomic across workers\n";
apcu_store('ctr', 0);
$pids = [];
foreach (range(1, 4) as $w) {
    $pid = pcntl_fork();
    if ($pid === 0) { for ($i = 0; $i < 500; $i++) apcu_inc('ctr'); exit(0); }
    $pids[] = $pid;
}
foreach ($pids as $p2) pcntl_waitpid($p2, $st);
var_dump(apcu_fetch('ctr'));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
Exception
-- counters
int(1)
bool(true)
int(6)
int(4)
int(4)
bool(false)
bool(false)
bool(true)
bool(false)
int(10)
bool(false)
-- entry
string(6) "made-e"
string(6) "made-e"
int(1)
-- info
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
-- atomic across workers
int(2000)
