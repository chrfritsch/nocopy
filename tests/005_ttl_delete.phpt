--TEST--
nocopy: TTL expiry and delete keep probe chains intact
--EXTENSIONS--
nocopy
--FILE--
<?php
nocopy_put('short', 'x', 1);
nocopy_put('long', 'y');
var_dump(nocopy_get('short'));
sleep(2);
var_dump(nocopy_get('short', $hit), $hit, nocopy_get('long'));

for ($i = 0; $i < 3000; $i++) nocopy_put("k$i", $i);
for ($i = 0; $i < 3000; $i += 3) nocopy_delete("k$i");
$bad = 0;
for ($i = 0; $i < 3000; $i++) {
    $v = nocopy_get("k$i", $hit);
    if ($i % 3 == 0 ? $hit : (!$hit || $v !== $i)) $bad++;
}
var_dump($bad);
for ($i = 0; $i < 3000; $i += 3) nocopy_put("k$i", -$i);
$bad = 0;
for ($i = 0; $i < 3000; $i++) {
    if (nocopy_get("k$i") !== ($i % 3 ? $i : -$i)) $bad++;
}
var_dump($bad, nocopy_delete('nope'));
try { nocopy_put('t', 1, -1); } catch (ValueError $e) { echo $e->getMessage(), "\n"; }
?>
--EXPECT--
string(1) "x"
NULL
bool(false)
string(1) "y"
int(0)
int(0)
bool(false)
nocopy_put(): Argument #3 ($ttl) must be greater than or equal to 0
