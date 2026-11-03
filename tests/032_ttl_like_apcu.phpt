--TEST--
stash: a ttl=1 entry lives through the next whole second, like APCu (1-2 s, not 0-1 s)
--EXTENSIONS--
stash
--FILE--
<?php
while (fmod(microtime(true), 1) > 0.05) usleep(1000);
stash_put('k', 1, 1);
$t0 = microtime(true);
foreach ([1.5, 2.2] as $at) {
    while (microtime(true) - $t0 < $at) usleep(1000);
    var_dump(stash_get('k'));
}
?>
--EXPECT--
int(1)
NULL
