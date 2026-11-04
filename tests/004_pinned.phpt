--TEST--
nocopy: a value fetched in this request survives replacement for the rest of the request
--EXTENSIONS--
nocopy
--INI--
nocopy.request_scope=1
apc.shm_size=2M
--FILE--
<?php
$orig = array_map(fn($i) => "orig-$i", range(1, 500));
nocopy_put('k', $orig);
$held = nocopy_get('k');
for ($i = 0; $i < 200; $i++) {       // far more than the arena holds: segments get recycled
    nocopy_put('k', array_map(fn($j) => "new-$i-$j", range(1, 500)));
}
var_dump($held === $orig);                  // the pinned segment was never reused
var_dump(nocopy_stats()['pinned_segments']); // ...and exactly that one is held back
?>
--EXPECT--
bool(true)
int(1)
