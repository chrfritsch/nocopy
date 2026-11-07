--TEST--
nocopy: nocopy_inc refuses a negative TTL like nocopy_put
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t081
--FILE--
<?php
foreach ([-10, -time()] as $ttl) {
    try {
        nocopy_inc('a', 1, $ttl);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
var_dump(nocopy_get('a', $hit), $hit, nocopy_inc('a'), nocopy_inc('a'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t081*')); ?>
--EXPECT--
nocopy_inc(): Argument #3 ($ttl) must be greater than or equal to 0
nocopy_inc(): Argument #3 ($ttl) must be greater than or equal to 0
NULL
bool(false)
int(1)
int(2)
