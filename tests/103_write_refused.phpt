--TEST--
nocopy: a tape that disagrees with the block refuses the store with a warning and leaves the shard usable
--SKIPIF--
<?php if (!getenv('NOCOPY_ASAN')) die('skip needs the checked build (./dev asan)'); ?>
--EXTENSIONS--
nocopy
--ENV--
NOCOPY_FAULT_TAPE=1
--INI--
nocopy.shm_name=/nocopy-t103
nocopy.shm_size=4M
--FILE--
<?php
var_dump(nocopy_put('a', ['x' => 'y']));
var_dump(nocopy_get('a'));
var_dump(nocopy_put('n', 5)); // no tape: unaffected
var_dump(nocopy_get('n'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t103*') ?: []); ?>
--EXPECTF--
Warning: nocopy_put(): nocopy: the value changed between planning and writing; not stored in %s on line %d
bool(false)
NULL
bool(true)
int(5)
