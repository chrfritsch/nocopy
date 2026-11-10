--TEST--
nocopy: an arena too small for its slot tables is a startup warning, then no cache
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t026
nocopy.shm_size=1M
nocopy.entries_hint=1000000
--FILE--
<?php var_dump(nocopy_put("k", 1), nocopy_get("k"), nocopy_shared_with()); ?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t026*')); ?>
--EXPECTF--
%AWarning: PHP Startup: nocopy: %d bytes needed for the slot tables and a few segments: nocopy.shm_size is too small or nocopy.entries_hint too large. The cache is off in this process: fetches miss, writes and clears fail in Unknown on line 0
bool(false)
NULL
string(4) "none"
