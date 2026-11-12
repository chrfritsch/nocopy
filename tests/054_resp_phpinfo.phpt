--TEST--
nocopy.resp: phpinfo() does not show the password
--EXTENSIONS--
nocopy
--SKIPIF--
<?php if (ini_get('nocopy.resp') === false) die('skip built without the RESP tier'); ?>
--INI--
nocopy.shm_name=/nocopy-t054
nocopy.resp=tcp://app:secret@127.0.0.1:1
--CONFLICTS--
resp
--FILE--
<?php
ob_start(); phpinfo(INFO_MODULES); $i = ob_get_clean();
var_dump(str_contains($i, 'secret'), str_contains($i, 'tcp://***@127.0.0.1:1'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t054*')); ?>
--EXPECT--
bool(false)
bool(true)
