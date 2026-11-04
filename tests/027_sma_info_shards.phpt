--TEST--
nocopy: apcu_sma_info() reports one segment per shard, with that slice's free blocks
--EXTENSIONS--
nocopy
--INI--
apc.enable_cli=1
apc.shm_size=32M
--FILE--
<?php
apcu_store('key', 'value');
$m = apcu_sma_info();
var_dump($m['num_seg'], count($m['block_lists']) === $m['num_seg']);
$full = $partial = 0;
foreach ($m['block_lists'] as $blocks) {
    if ($blocks === [['size' => (int) $m['seg_size'], 'offset' => 0]]) {
        $full++;
    } else {
        $partial++;
        foreach ($blocks as $b) {
            assert($b['offset'] + $b['size'] <= $m['seg_size']);
        }
    }
}
var_dump($full, $partial, $m['avail_mem'] < $m['num_seg'] * $m['seg_size'], array_keys(apcu_sma_info(true)));
?>
--EXPECT--
int(4)
bool(true)
int(3)
int(1)
bool(true)
array(3) {
  [0]=>
  string(7) "num_seg"
  [1]=>
  string(8) "seg_size"
  [2]=>
  string(9) "avail_mem"
}
