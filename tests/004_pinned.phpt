--TEST--
stash: a value fetched in this request survives replacement until checkpoint
--EXTENSIONS--
stash
--INI--
stash.request_scope=1
apc.shm_size=2M
--FILE--
<?php
$orig = array_map(fn($i) => "orig-$i", range(1, 500));
stash_put('k', $orig);
$held = stash_get('k');
for ($i = 0; $i < 200; $i++) {       // far more than the arena holds: segments get recycled
    stash_put('k', array_map(fn($j) => "new-$i-$j", range(1, 500)));
}
var_dump($held === $orig);                  // the pinned segment was never reused
var_dump(stash_stats()['pinned_segments']); // ...and exactly that one is held back
unset($held);
stash_checkpoint();
var_dump(stash_stats()['pinned_segments']);
?>
--EXPECT--
bool(true)
int(1)
int(0)
