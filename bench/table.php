<?php
$cfgs = ['apcu_php' => 'APCu (php ser.)', 'apcu_default' => 'APCu (default)', 'stash_copy' => 'stash (copy)', 'stash_zero' => 'stash (zero-copy)'];
$dir = $argv[1] ?? "results"; $d = []; $unit = [];
foreach ($cfgs as $c => $_) foreach (file(__DIR__ . "/$dir/$c.jsonl") as $l) { $j = json_decode($l, true); $d[$j['name']][$c] = $j['v']; $unit[$j['name']] = $j['unit']; }
printf("%-42s %16s %16s %14s %17s\n", 'benchmark', ...array_values($cfgs));
foreach ($d as $name => $row) {
    $u = $unit[$name]; $lower = $u === 'ns/op';
    $apcuBest = $lower ? min($row['apcu_php'], $row['apcu_default']) : max($row['apcu_php'], $row['apcu_default']);
    $cells = array_map(fn($c) => number_format($row[$c], $row[$c] < 1 ? 3 : ($row[$c] < 100 ? 1 : 0)), array_keys($cfgs));
    $stashBest = $lower ? min($row['stash_copy'], $row['stash_zero']) : max($row['stash_copy'], $row['stash_zero']);
    $copyVs = $lower ? $apcuBest / $row['stash_copy'] : $row['stash_copy'] / $apcuBest;
    $zeroVs = $lower ? $apcuBest / $row['stash_zero'] : $row['stash_zero'] / $apcuBest;
    vprintf("%-42s %16s %16s %14s %17s  copy %5.1fx | zero %6.1fx   [%s]\n", array_merge([$name], $cells, [$copyVs, $zeroVs, $u]));
}
