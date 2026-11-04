--TEST--
nocopy.valkey: phpinfo() does not show the password
--EXTENSIONS--
nocopy
--INI--
nocopy.valkey=tcp://app:secret@127.0.0.1:1
--FILE--
<?php
ob_start(); phpinfo(INFO_MODULES); $i = ob_get_clean();
var_dump(str_contains($i, 'secret'), str_contains($i, 'tcp://***@127.0.0.1:1'));
?>
--EXPECT--
bool(false)
bool(true)
