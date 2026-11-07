--TEST--
nocopy: DateTime and DateTimeImmutable stored as timestamp and zone: same as unserialize() for every zone type, DST, microseconds; fallbacks
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t075
date.timezone=UTC
--FILE--
<?php
require __DIR__ . '/serve.inc';
$test = <<<'CODE'
error_reporting(E_ALL & ~E_DEPRECATED); // dynamic properties on DateTime
class MyDate extends DateTime { public $tag = 't'; }
$zones = ['Europe/Berlin', 'America/New_York', 'UTC', 'Asia/Kathmandu', '+05:30', '-11:00', 'CEST', 'EST', 'Z'];
$times = ['2026-03-29 01:59:59.999999', '2026-03-29 03:00:00', '2026-10-25 02:30:00.5', '1901-12-13 20:45:52', '1969-12-31 23:59:59.25', '2400-02-29 12:00:00'];
$vals = [];
foreach ($zones as $z) {
    foreach ($times as $t) {
        $vals[] = new DateTime($t, new DateTimeZone($z));
        $vals[] = new DateTimeImmutable($t, new DateTimeZone($z));
    }
}
$vals[] = new DateTime('@1700000000.123456');
$vals[] = (new DateTime('2026-06-01 12:00', new DateTimeZone('Europe/Berlin')))->setTimezone(new DateTimeZone('-03:00'));
$vals[] = new MyDate('2026-01-01 00:00', new DateTimeZone('Europe/Paris'));
$shared = new DateTimeImmutable('2026-05-05 05:05:05', new DateTimeZone('Europe/Berlin'));
$vals[] = [$shared, $shared];
nocopy_put('dates', $vals);
for ($i = 0; $i < 3; $i++) { // 3: copy mode keeps a template from the second fetch on
    $got = nocopy_get('dates');
    foreach ($vals as $k => $v) {
        if (serialize($got[$k]) !== serialize($v)) echo "#$i $k DIFFERS: ", serialize($got[$k]), ' vs ', serialize($v), "\n";
    }
}
$g = nocopy_get('dates');
$last = end($g);
var_dump($last[0] === $last[1], get_class($g[0]), get_class($g[1]));
// the zone still works after the fetch: across the DST change
$d = $g[0]; // Berlin, 01:59:59.999999 before the switch
echo $d->modify('+1 second')->format('Y-m-d H:i:s.u T P e'), "\n";
echo $g[2 * count($times)]->getTimezone()->getName(), ' ', $g[2 * count($times)]->getTimestamp(), "\n";
// fallbacks: uninitialized (serialize() throws), dynamic properties (serialized whole)
try { var_dump(nocopy_put('u', [(new ReflectionClass('DateTime'))->newInstanceWithoutConstructor()])); } catch (Error $e) { echo $e->getMessage(), "\n"; }
$dyn = new DateTime('2026-01-01', new DateTimeZone('Europe/Berlin'));
$dyn->extra = 1;
nocopy_put('dyn', [$dyn]);
var_dump(nocopy_get('dyn')[0]->extra, nocopy_get('dyn')[0]->format('c'));
CODE;
echo "-- copy mode\n";
eval($test);
echo "-- zero-copy\n";
echo serve('<?php ' . $test, ['date.timezone' => 'UTC']);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t075*')); ?>
--EXPECTF--
-- copy mode
bool(true)
string(8) "DateTime"
string(17) "DateTimeImmutable"
2026-03-29 03:00:00.999999 CEST +02:00 Europe/Berlin
America/New_York 1774763999
%s has not been correctly initialized%s
int(1)
string(25) "2026-01-01T00:00:00+01:00"
-- zero-copy
bool(true)
string(8) "DateTime"
string(17) "DateTimeImmutable"
2026-03-29 03:00:00.999999 CEST +02:00 Europe/Berlin
America/New_York 1774763999
%s has not been correctly initialized%s
int(1)
string(25) "2026-01-01T00:00:00+01:00"
