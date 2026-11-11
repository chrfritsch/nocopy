--TEST--
nocopy: after workers churned (zero-copy and copy mode: pins, kept copies, merges, eviction, leases), a clear returns every segment
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t113
nocopy.shm_size=4M
--FILE--
<?php
require __DIR__ . '/serve.inc';
require __DIR__ . '/empty.inc';
$work = <<<'CODE'
<?php
mt_srand(113);
$held = [];
$o = new stdClass; $o->self = $o; $o->list = range(1, 50);
for ($i = 0; $i < 4000; $i++) {
    $k = 'k' . mt_rand(0, 200);
    switch (mt_rand(0, 9)) {
        case 0: case 1: nocopy_put($k, mt_rand(0, 3) ? array_fill(0, mt_rand(1, 3000), "v$i") : str_repeat('x', mt_rand(1, 200000))); break;
        case 2: nocopy_put($k, [$o, 'n' => $i, 'o' => $o]); break;
        case 3: $v = nocopy_get($k); if (mt_rand(0, 3) === 0) $held[$i % 50] = $v; break;
        case 4: nocopy_get_multiple([$k, 'k' . mt_rand(0, 200), 'c']); break;
        case 5: nocopy_delete($k); break;
        case 6: nocopy_inc('c'); nocopy_cas('c', 1, 2); nocopy_add($k, $i, 1); break;
        case 7: nocopy_put_multiple(['m1' => range(0, $i % 100), 'm2' => "s$i"]); break;
        case 8: nocopy_remember("r$k", fn() => array_fill(0, 100, $i), 60); break;
        default: foreach (nocopy_keys('k1') as $kk => $_) nocopy_get($kk);
    }
}
echo "done ", count($held) > 0 ? 'held' : 'none', "\n";
CODE;
// zero-copy: what a request pinned goes at its end, checked from the same live server process
echo serve([$work, '<?php require ' . var_export(__DIR__ . '/empty.inc', true) . '; var_dump(arena_leftovers());']), "\n";
$f = tempnam(sys_get_temp_dir(), 'nc113');
file_put_contents($f, $work);
$p = proc_open(php_argv(ini_get_all('nocopy', false), [$f]), [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
echo stream_get_contents($pipes[1]), stream_get_contents($pipes[2]); // copy mode: kept copies, until the process ends
proc_close($p);
unlink($f);
var_dump(arena_leftovers());
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t113*')); ?>
--EXPECT--
done held
array(0) {
}

done held
array(0) {
}
