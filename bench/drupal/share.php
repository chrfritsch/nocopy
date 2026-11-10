<?php
// drush php:script: how much identical arrays shared arena-wide would save on the database cache rows (zero-copy layout modelled, no arena).
// Sizes: lo counts strings <= 64 bytes as free (string table, atoms), hi counts them at every occurrence. Arrays holding objects are not shared.
const VSTR = 64;
function str_sz(string $s): int { return (24 + strlen($s) + 1 + 7) & ~7; }
function arr_sz(array $a): int
{
    $n = count($a);
    if (array_is_list($a)) {
        return 56 + 16 * $n;
    }
    for ($slots = 4; $slots < 2 * $n; $slots <<= 1);
    return 56 + 4 * $slots + 32 * $n;
}
// [lo, hi, hash|null, children]
function node($v): array
{
    if (is_string($v)) {
        $s = str_sz($v);
        return [strlen($v) > VSTR ? $s : 0, $s, strlen($v) > (int) (getenv('STRMAX') ?: PHP_INT_MAX) ? null : 's' . $v, []];
    }
    if (is_object($v)) {
        if (isset($GLOBALS["objs"][spl_object_id($v)])) {
            return [0, 0, null, []]; // a back reference
        }
        $GLOBALS["objs"][spl_object_id($v)] = $v;
        $p = get_mangled_object_vars($v);
        $lo = $hi = 8 + 16 * count($p);
        $kids = [];
        foreach ($p as $x) {
            $k = node($x);
            $lo += $k[0]; $hi += $k[1];
            if (is_array($x) || is_object($x)) {
                $kids[] = $k;
            }
        }
        return [$lo, $hi, null, $kids];
    }
    if (!is_array($v)) {
        return [0, 0, 'v' . serialize($v), []];
    }
    if (!$v) {
        return [0, 0, 'e', []];
    }
    $lo = $hi = arr_sz($v);
    $h = '';
    $share = true;
    $kids = [];
    foreach ($v as $key => $x) {
        if (is_string($key)) {
            $ks = str_sz($key);
            $hi += $ks;
            $lo += strlen($key) > VSTR ? $ks : 0;
        }
        $k = node($x);
        $lo += $k[0]; $hi += $k[1];
        $share = $share && $k[2] !== null;
        $h .= $key . "\0" . ($k[2] ?? '') . "\1";
        if (is_array($x) || is_object($x)) {
            $kids[] = $k;
        }
    }
    $h = $share ? hash('xxh128', $h, true) : null;
    if ($h !== null && isset($GLOBALS['want'][$h]) && !isset($GLOBALS['prev'][$h])) {
        $GLOBALS['prev'][$h] = $GLOBALS['bin'] . ' ' . substr(json_encode($v, JSON_UNESCAPED_SLASHES | JSON_PARTIAL_OUTPUT_ON_ERROR), 0, 300);
    }
    return [$lo, $hi, $h, $kids];
}
// first occurrence stays (its children may be shared), a later one is a pointer
function dedup(array $n, array &$seen, array &$saved, array &$by, bool $track = false): void
{
    if ($n[2] !== null && is_array($n[3]) && $n[0] + $n[1] > 0) {
        if (($seen[$n[2]] ?? 0) >= (int) (getenv('KEEP') ?: 1)) {
            $saved[0] += $n[0]; $saved[1] += $n[1];
            $b = $n[1] <= 128 ? '<=128' : ($n[1] <= 1024 ? '<=1K' : ($n[1] <= 16384 ? '<=16K' : '>16K'));
            $by[$b][0] = ($by[$b][0] ?? 0) + 1;
            $by[$b][1] = ($by[$b][1] ?? 0) + $n[1];
            if ($track) {
                $GLOBALS['sav'][$n[2]][0] = ($GLOBALS['sav'][$n[2]][0] ?? 0) + 1;
                $GLOBALS['sav'][$n[2]][1] = ($GLOBALS['sav'][$n[2]][1] ?? 0) + $n[1];
            }
            return;
        }
        $seen[$n[2]] = ($seen[$n[2]] ?? 0) + 1;
    }
    foreach ($n[3] as $k) {
        dedup($k, $seen, $saved, $by, $track);
    }
}
$db = \Drupal::database();
$each = function (callable $f) use ($db) {
    foreach ($db->query("SHOW TABLES LIKE 'cache%'")->fetchCol() as $t) {
        if ($t === 'cachetags') {
            continue;
        }
        foreach ($db->query("SELECT cid, data, serialized FROM {$t}") as $r) {
            $GLOBALS['objs'] = [];
            $GLOBALS['bin'] = $t;
            $f($t, node($r->serialized ? unserialize($r->data) : $r->data));
        }
    }
};
$tot = [0, 0]; $glob = [0, 0]; $row = [0, 0]; $byr = []; $seen = []; $bins = []; $rows = 0;
$GLOBALS['by'] = $GLOBALS['sav'] = $GLOBALS['want'] = $GLOBALS['prev'] = [];
$each(function ($t, $n) use (&$tot, &$glob, &$row, &$byr, &$seen, &$bins, &$rows) {
        $rows++;
        $tot[0] += $n[0]; $tot[1] += $n[1];
        $g = [0, 0];
        dedup($n, $seen, $g, $GLOBALS['by'], true);
        $glob[0] += $g[0]; $glob[1] += $g[1];
        $rs = [];
        $x = [0, 0];
        dedup($n, $rs, $x, $byr);
        $row[0] += $x[0]; $row[1] += $x[1];
        $bins[$t][0] = ($bins[$t][0] ?? 0) + $n[1];
        $bins[$t][1] = ($bins[$t][1] ?? 0) + $g[1];
});
$by = $GLOBALS['by'];
$pc = fn($a, $b) => $b ? sprintf('%.1f%%', 100 * $a / $b) : '-';
printf("rows\t%d\tmodel_lo\t%d\tmodel_hi\t%d\n", $rows, $tot[0], $tot[1]);
printf("arena_wide\tlo\t%d\t%s\thi\t%d\t%s\n", $glob[0], $pc($glob[0], $tot[0]), $glob[1], $pc($glob[1], $tot[1]));
printf("within_row\tlo\t%d\t%s\thi\t%d\t%s\n", $row[0], $pc($row[0], $tot[0]), $row[1], $pc($row[1], $tot[1]));
foreach (['<=128', '<=1K', '<=16K', '>16K'] as $b) {
    printf("shared %s\tcount\t%d\tbytes_hi\t%d\n", $b, $by[$b][0] ?? 0, $by[$b][1] ?? 0);
}
uasort($bins, fn($a, $b) => $b[0] <=> $a[0]);
foreach ($bins as $t => [$s, $g]) {
    printf("bin\t%s\tbytes_hi\t%d\tsaved_hi\t%d\t%s\n", $t, $s, $g, $pc($g, $s));
}
// the arrays that save most, previewed in a second pass (bin of the first occurrence, JSON cut at 300 bytes)
$sav = $GLOBALS['sav'];
uasort($sav, fn($a, $b) => $b[1] <=> $a[1]);
$top = array_slice($sav, 0, 40, true);
$GLOBALS['want'] = $top;
$each(fn() => null);
foreach ($top as $h => [$c, $s]) {
    printf("top\tsaved\t%d\trepeats\t%d\tsize\t%d\t%s\n", $s, $c, intdiv($s, $c), $GLOBALS['prev'][$h] ?? '?');
}
$db_ = $big = 0;
foreach ($sav as [$c, $s]) {
    $db_ += intdiv($s, $c);
    $big += intdiv($s, $c) >= 128 ? $s : 0;
}
printf("distinct_shared\t%d\tdistinct_bytes\t%d\tsaved_by_ge128\t%d\n", count($sav), $db_, $big);
