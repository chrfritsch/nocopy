<?php
// drush php:script: Drupal cache rows, bytes when strings <= 64 B and arrays >= 128 B are deduped per row, per segment, or arena-wide.
const VSTR = 64, AMIN = 128, AHASH = 256;
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
// [own bytes, short strings, kids, hash|null (arrays only), total bytes]
function node($v): array
{
    if (is_string($v)) {
        return strlen($v) <= VSTR ? [0, [$v], [], null, str_sz($v)] : [str_sz($v), [], [], null, str_sz($v)];
    }
    if (is_object($v)) {
        if (isset($GLOBALS['objs'][spl_object_id($v)])) {
            return [0, [], [], null, 0];
        }
        $GLOBALS['objs'][spl_object_id($v)] = $v;
        $p = get_mangled_object_vars($v);
        $n = [8 + 16 * count($p), [], [], null, 0];
        foreach ($p as $x) {
            add($n, node($x), $x);
        }
        $n[4] += $n[0];
        return $n;
    }
    if (!is_array($v)) {
        return [0, [], [], 'v' . serialize($v), 0];
    }
    if (!$v) {
        return [0, [], [], 'e', 0];
    }
    $n = [arr_sz($v), [], [], null, 0];
    $h = '';
    $share = true;
    foreach ($v as $key => $x) {
        if (is_string($key)) {
            if (strlen($key) <= VSTR) {
                $n[1][] = $key;
            } else {
                $n[0] += str_sz($key);
            }
            $n[4] += str_sz($key);
        }
        $k = node($x);
        add($n, $k, $x);
        if (is_string($x)) {
            $share = $share && strlen($x) <= AHASH;
            $h .= $key . "\0s" . $x . "\1";
        } else {
            $share = $share && $k[3] !== null;
            $h .= $key . "\0" . $k[3] . "\1";
        }
    }
    $n[4] += $n[0];
    $n[3] = $share ? 'a' . hash('xxh128', $h, true) : null;
    return $n;
}
function add(array &$n, array $k, $x): void
{
    if (is_string($x)) {
        $n[1] = array_merge($n[1], $k[1]);
        $n[0] += $k[0];
        $n[4] += $k[4];
    } elseif (is_array($x) || is_object($x)) {
        $n[2][] = $k;
        $n[4] += $k[4];
    }
}
// bytes written given the scope's $seen; new keys go to $added (for rollback)
function cost(array $n, array &$seen, array &$added, bool $arrays): int
{
    if ($arrays && $n[3] !== null && $n[4] >= AMIN) {
        if (isset($seen[$n[3]])) {
            return 0;
        }
        $seen[$n[3]] = 1; $added[] = $n[3];
    }
    $b = $n[0];
    foreach ($n[1] as $s) {
        $k = 's' . $s;
        if (!isset($seen[$k])) {
            $seen[$k] = 1; $added[] = $k;
            $b += str_sz($s);
        }
    }
    foreach ($n[2] as $k) {
        $b += cost($k, $seen, $added, $arrays);
    }
    return $b;
}
$rows = [];
$db = \Drupal::database();
foreach ($db->query("SHOW TABLES LIKE 'cache%'")->fetchCol() as $t) {
    if ($t === 'cachetags') {
        continue;
    }
    foreach ($db->query("SELECT cid, data, serialized, created FROM {$t} ORDER BY created") as $r) {
        $GLOBALS['objs'] = [];
        $rows[] = [$t . ':' . $r->cid, node($r->serialized ? unserialize($r->data) : $r->data), (float) $r->created];
    }
}
usort($rows, fn($a, $b) => $a[2] <=> $b[2]);
$hdr = fn($cid) => 40 + ((strlen($cid) + 7) & ~7);
$out = function (string $name, int $bytes, int $base) {
    printf("%s\t%d\t%.1f%%\n", $name, $bytes, 100 * ($base - $bytes) / $base);
};
// per row: the per-entry string table, no arrays
$base = 0;
foreach ($rows as [$cid, $n]) {
    $s = []; $a = [];
    $base += $hdr($cid) + cost($n, $s, $a, false);
}
printf("rows\t%d\n", count($rows));
$out('per_row_strings', $base, $base);
foreach ([false, true] as $arrays) {
    $s = []; $a = []; $b = 0;
    foreach ($rows as [$cid, $n]) {
        $b += $hdr($cid) + cost($n, $s, $a, $arrays);
    }
    $out('arena' . ($arrays ? '_strings_arrays' : '_strings'), $b, $base);
}
// per segment: rows go to shards by key hash, each shard fills one segment at a time
foreach ([1, 4] as $nsh) {
    foreach ([65536, 131072, 262144, 1048576, 4194304] as $seg) {
        foreach ([false, true] as $arrays) {
            $sh = array_fill(0, $nsh, [[], 0]);
            $b = 0;
            foreach ($rows as [$cid, $n]) {
                $i = crc32($cid) % $nsh;
                [$seen, $used] = $sh[$i];
                $added = [];
                $c = $hdr($cid) + cost($n, $seen, $added, $arrays);
                if ($used + $c > $seg) {
                    $seen = []; $added = [];
                    $c = $hdr($cid) + cost($n, $seen, $added, $arrays);
                    if ($c > $seg / 2) {
                        $seen = $sh[$i][0];
                        $b += $c;
                        continue;
                    }
                    $used = 0;
                }
                $sh[$i] = [$seen, $used + $c];
                $b += $c;
            }
            $out(sprintf('seg_%dK_shards%d%s', $seg >> 10, $nsh, $arrays ? '_strings_arrays' : '_strings'), $b, $base);
        }
    }
}
