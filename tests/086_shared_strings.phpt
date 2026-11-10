--TEST--
nocopy: short strings met twice are stored once arena-wide; a generation's half is wiped only once nothing references it
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t086
nocopy.shm_size=2M
--FILE--
<?php
require __DIR__ . '/serve.inc';
$lib = <<<'PHP'
$st = fn($k) => nocopy_stats()[$k];
$batch = fn($t, $n) => array_map(fn($i) => str_pad("$t-$i", 60, "."), range(0, $n - 1)); // 96-byte atoms: 680 fill a half
$fill = function ($t, $n) use ($batch) { // the second put admits what the first one met
    $v = $batch($t, $n);
    nocopy_put("$t-1", $v);
    nocopy_put("$t-2", $v);
    return $v;
};
PHP;
eval($lib);

// shared across entries: once admitted, an entry holds none of their bytes
$row = ['language' => 'languages:language_interface', 'tags' => ['user.permissions', 'theme', 'config:block_list']];
$u = $st('used'); nocopy_put('r1', $row); $first = $st('used') - $u;
nocopy_put('r2', $row);
$u = $st('used'); nocopy_put('r3', $row); $third = $st('used') - $u;
var_dump($third < $first, nocopy_get('r1') === $row, nocopy_get('r3') === $row);
$copy = nocopy_get('r3'); $copy['tags'][] = 'x'; // copy on write: the atoms stay as they were
var_dump(nocopy_get('r3') === $row);

// generation 2 fills, then the empty generation 1's half is wiped for generation 3
$a = $fill('a', 480);
nocopy_put('old', $a); // references generation 2
$fill('b', 1200);
var_dump($st('shared_strings_rotations'));

// a zero-copy reader holds 'old' (its strings are generation 2's atoms): the half stays, also past a clear
echo serve("<?php $lib" . <<<'PHP'
    $v = nocopy_get('old');
    nocopy_clear();
    sleep(1); // a rotation that found generation 2 referenced waits a second
    $fill('c', 1600);
    var_dump($st('shared_strings_rotations'), $v === $batch('a', 480));
    PHP);

// unpinned and cleared: the next fill rotates again, and every value reads back intact
nocopy_clear();
sleep(1);
$d = $fill('d', 1600);
nocopy_put('new', $d);
var_dump($st('shared_strings_rotations'), nocopy_get('d-2') === $d, nocopy_get('new') === $d);

?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t086*')); ?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
int(1)
int(1)
bool(true)
int(2)
bool(true)
bool(true)
