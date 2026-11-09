--TEST--
nocopy: a string that is both a value and a key keeps its hash when a merge moves the entry (extract(), array_replace_recursive() look keys up by it)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t091
nocopy.shm_size=4M
--FILE--
<?php
require __DIR__ . '/serve.inc';
echo serve(<<<'CODE'
<?php
function vals() {
    return [
        'vk' => ['x' => 'name', 'name' => ['b' => 2]], // the value first, then the key: one string in the block
        'kv' => ['a' => 'a', 'n' => 1],                // one byte: the key first, then the value
    ];
}
function check($label) {
    $name = ['a' => 1];
    extract(nocopy_get('vk'));
    $a = 'old';
    extract(nocopy_get('kv'));
    echo $label, ': ', json_encode([$name, $a, array_replace_recursive(['name' => ['a' => 1]], nocopy_get('vk'))['name']]), "\n";
}
foreach (vals() as $k => $v) nocopy_put($k, $v, 0);
check('fresh');
$moved = nocopy_stats()['evacuated_bytes'];
for ($i = 0; $i < 6000; $i++) { // churn: merges move the read values forward
    nocopy_put("t$i", str_repeat('z', 2000));
    if ($i % 100 == 0) foreach (vals() as $k => $_) nocopy_get($k);
}
var_dump(nocopy_stats()['evacuated_bytes'] > $moved, nocopy_get('vk') === vals()['vk'], nocopy_get('kv') === vals()['kv']);
check('moved');
CODE);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t091*')); ?>
--EXPECT--
fresh: [{"b":2},"a",{"a":1,"b":2}]
bool(true)
bool(true)
bool(true)
moved: [{"b":2},"a",{"a":1,"b":2}]
