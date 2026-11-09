<?php
// drush php:script, with nocopy loaded: the database cache rows as a read-through workload (get; on miss put) over an arena smaller than they are.
// args: zipf (alpha 0.99 over the rows, ranks shuffled) | loop (rows in order, cyclically); prints one tab-separated line.
$wl = $extra[0] ?? 'zipf';
$db = \Drupal::database();
$rows = [];
foreach ($db->query("SHOW TABLES LIKE 'cache%'")->fetchCol() as $t) {
    if ($t === 'cachetags') {
        continue;
    }
    foreach ($db->query("SELECT cid, data, serialized FROM {$t}") as $r) {
        $rows[] = ["$t:$r->cid", $r->serialized ? unserialize($r->data) : $r->data];
    }
}
$n = count($rows);
mt_srand(42);
shuffle($rows);
if ($wl === 'zipf') {
    $cdf = []; $s = 0.0;
    for ($i = 1; $i <= $n; $i++) { $s += 1 / $i ** 0.99; $cdf[] = $s; }
    $next = function () use ($cdf, $s, $n) {
        $r = mt_rand() / mt_getrandmax() * $s;
        for ($lo = 0, $hi = $n - 1; $lo < $hi;) { $m = ($lo + $hi) >> 1; if ($cdf[$m] < $r) $lo = $m + 1; else $hi = $m; }
        return $lo;
    };
} else {
    $i = 0;
    $next = function () use (&$i, $n) { return $i++ % $n; };
}
$reqs = 20 * $n; $hits = $cnt = $failed = 0;
for ($q = 0; $q < $reqs; $q++) {
    [$k, $v] = $rows[$next()];
    nocopy_get($k, $hit);
    if (!$hit) $failed += !nocopy_put($k, $v);
    if ($q >= $reqs / 4) { $cnt++; $hits += $hit; } // first quarter warms up
}
$st = nocopy_stats();
printf("%s\t%s\trows\t%d\thits_pct\t%.1f\tevicted\t%d\tfailed\t%d\tshared_strings\t%d\n", ini_get('nocopy.shm_size'), $wl, $n, 100 * $hits / $cnt, $st['evicted'], $failed, $st['shared_strings'] ?? 0);
