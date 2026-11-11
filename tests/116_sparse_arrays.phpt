--TEST--
nocopy: arrays with more holes than elements are stored at the size of their elements (were copied slot for slot: a packed array left with one element of a million did not fit)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t116
nocopy.shm_size=32M
--FILE--
<?php
$a = range(1, 1000000); for ($i = 0; $i < 999999; $i++) unset($a[$i]);
$b = range(0, 99); foreach ($b as $k => $_) if ($k % 10) unset($b[$k]);
$c = []; for ($i = 0; $i < 1000; $i++) $c["s$i"] = $i; for ($i = 0; $i < 990; $i++) unset($c["s$i"]); $c[7] = 'int';
$d = ['keep' => [1]]; for ($i = 0; $i < 50; $i++) $d["x$i"] = new stdClass; for ($i = 0; $i < 50; $i++) unset($d["x$i"]); $d['o'] = new ArrayObject([1]);
foreach (compact('a', 'b', 'c', 'd') as $k => $v) {
    $ok = nocopy_put($k, $v);
    $g = nocopy_get($k);
    $u = unserialize(serialize($v));
    $g[] = 'app'; $u[] = 'app'; $g['n'] = 1; $u['n'] = 1;
    printf("%s: %s %s %s %s\n", $k, var_export($ok, true), var_export(serialize(nocopy_get($k)) === serialize($v), true),
        var_export($g == $u && array_keys($g) === array_keys($u), true), nocopy_keys($k)[$k]['size'] < 2048 ? 'small' : 'big');
}
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t116*')); ?>
--EXPECT--
a: true true true small
b: true true true small
c: true true true small
d: true true true small
