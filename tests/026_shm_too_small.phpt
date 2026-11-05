--TEST--
nocopy: an arena too small for its slot tables fails startup
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t026
nocopy.shm_size=1M
nocopy.entries_hint=1000000
--FILE--
Irrelevant
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t026*')); ?>
--EXPECTF--
%AFatal error: nocopy: %d bytes needed for the slot tables and a few segments: nocopy.shm_size is too small or nocopy.entries_hint too large in Unknown on line 0
