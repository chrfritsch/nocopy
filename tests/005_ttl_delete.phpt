--TEST--
stash: TTL expiry and delete keep probe chains intact
--EXTENSIONS--
stash
--FILE--
<?php
stash_put('short', 'x', 1);
stash_put('long', 'y');
var_dump(stash_get('short'));
sleep(2);
var_dump(stash_get('short', $hit), $hit, stash_get('long'));

for ($i = 0; $i < 3000; $i++) stash_put("k$i", $i);
for ($i = 0; $i < 3000; $i += 3) stash_delete("k$i");
$bad = 0;
for ($i = 0; $i < 3000; $i++) {
    $v = stash_get("k$i", $hit);
    if ($i % 3 == 0 ? $hit : (!$hit || $v !== $i)) $bad++;
}
var_dump($bad);
for ($i = 0; $i < 3000; $i += 3) stash_put("k$i", -$i);
$bad = 0;
for ($i = 0; $i < 3000; $i++) {
    if (stash_get("k$i") !== ($i % 3 ? $i : -$i)) $bad++;
}
var_dump($bad, stash_delete('nope'));
try { stash_put('t', 1, -1); } catch (ValueError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECT--
string(1) "x"
NULL
bool(false)
string(1) "y"
int(0)
int(0)
bool(false)
stash_put(): Argument #3 ($ttl) must be greater than or equal to 0
