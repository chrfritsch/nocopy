<?php
// nocopy_delete in a loop vs nocopy_delete_multiple: µs per batch of N existing keys, median of 21. Run with and without nocopy.resp.
// usage: php -d nocopy.shm_name=.. [-d nocopy.resp=..] delete.php
foreach ([1, 10, 100, 1000] as $n) {
    $keys = array_map(fn ($i) => "cache::bin::entity:node:$i", range(1, $n));
    $vals = array_fill_keys($keys, ['data' => str_repeat('x', 200)]);
    foreach (['loop' => fn () => array_map('nocopy_delete', $keys), 'multiple' => fn () => nocopy_delete_multiple($keys)] as $how => $fn) {
        $t = [];
        for ($r = 0; $r < 21; $r++) {
            nocopy_put_multiple($vals);
            $s = hrtime(true);
            $fn();
            $t[] = (hrtime(true) - $s) / 1e3;
        }
        sort($t);
        printf("%-5d %-9s %10.1f µs\n", $n, $how, $t[10]);
    }
}
