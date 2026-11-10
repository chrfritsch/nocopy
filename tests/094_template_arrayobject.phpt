--TEST--
nocopy: copy mode's template is not changed through an object built from it (an ArrayObject iterated by reference writes its array in place)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t094
--FILE--
<?php
nocopy_put('ao', new ArrayObject([false, 'k' => 7, 'n' => [1, 2]]));
for ($i = 0; $i < 4; $i++) { // the second fetch keeps a template, later ones are built from it
    $o = nocopy_get('ao');
    echo json_encode($o->getArrayCopy()), "\n";
    foreach ($o as &$p) {
        $p = null;
    }
    unset($p, $o);
}
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t094*')); ?>
--EXPECT--
{"0":false,"k":7,"n":[1,2]}
{"0":false,"k":7,"n":[1,2]}
{"0":false,"k":7,"n":[1,2]}
{"0":false,"k":7,"n":[1,2]}
