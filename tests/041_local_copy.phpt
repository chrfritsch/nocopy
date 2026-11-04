--TEST--
stash: copy mode keeps private copies for the request; modifying, overwriting or deleting never shows a stale value
--EXTENSIONS--
stash
--INI--
stash.request_scope=0
stash.local_size=64K
--FILE--
<?php
$rows = [['id' => 1, 'tags' => ['a', 'b']], ['id' => 2, 'tags' => ['c']]];
stash_put('rows', $rows);
stash_put('s', str_repeat('x', 1000));

$a = stash_get('rows');
$b = stash_get('rows');
$a[0]['tags'][] = 'z';         // $b is the kept copy (second fetch): COW keeps it as stored
$b[1]['id'] = 99;
var_dump(stash_get('rows') === $rows, $a[0]['tags'], $b[1]['id']);

stash_get('s');
$s = stash_get('s'); $s[0] = 'y'; // second fetch: $s is the kept copy
var_dump(stash_get('s') === str_repeat('x', 1000));

stash_put('rows', [1, 2, 3]);  // new version: the kept copy is not reused
var_dump(stash_get('rows'));
stash_delete('rows');
var_dump(stash_get('rows'));

// more than the budget: flushes, values stay right
for ($i = 0; $i < 300; $i++) stash_put("k$i", array_fill(0, 20, $i));
$bad = 0;
for ($r = 0; $r < 3; $r++) for ($i = 0; $i < 300; $i++) $bad += stash_get("k$i") !== array_fill(0, 20, $i);
var_dump($bad);
?>
--EXPECT--
bool(true)
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
  [2]=>
  string(1) "z"
}
int(99)
bool(true)
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
}
NULL
int(0)
