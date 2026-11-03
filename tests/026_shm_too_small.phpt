--TEST--
stash: an arena too small for its slot tables fails startup with APCu's message
--EXTENSIONS--
stash
--INI--
stash.apcu_compat=0
stash.shm_size=1M
stash.entries_hint=1000000
--FILE--
Irrelevant
--EXPECTF--
%A: Unable to allocate %d bytes of shared memory for cache structures. Either stash.shm_size is too small or stash.entries_hint too large in Unknown on line 0
