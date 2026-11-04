--TEST--
nocopy.serializer=igbinary: objects, __serialize, cycles and references round-trip; serialized entries are not counters
--EXTENSIONS--
nocopy
igbinary
--INI--
apc.enable_cli=1
nocopy.serializer=igbinary
--FILE--
<?php
class P { public $a = [1, 2]; public ?P $self = null; }
class S { public $x = 1; function __serialize(): array { return ['y' => 7]; } function __unserialize(array $d): void { $this->x = $d['y']; } }
$p = new P; $p->self = $p;
$v = ['p' => $p, 's' => new S, 'arr' => [1, 'two', 3.0]];
$v['ref'] = &$v['arr'];
var_dump(ini_get('nocopy.serializer'));
var_dump(nocopy_put('k', $v, 0, true));
$r = nocopy_get('k');
var_dump($r['p']->self === $r['p'], $r['s']->x, $r['arr'] === [1, 'two', 3.0]);
$r['ref'][] = 4;
var_dump(count($r['arr']));
apcu_store('o', new ArrayObject([1, 2]));
var_dump(apcu_fetch('o')->count());
var_dump(apcu_inc('o'));
$o = new ArrayObject(array_fill(0, 1000, str_repeat('x', 100)));
apcu_store('dedup', $o);
foreach (new APCUIterator('/^dedup$/') as $e) var_dump($e['mem_size'] < 20000); // igbinary dedups strings, serialize() would need ~110 KB
?>
--EXPECT--
string(8) "igbinary"
bool(true)
bool(true)
int(7)
bool(true)
int(4)
int(2)
bool(false)
bool(true)
