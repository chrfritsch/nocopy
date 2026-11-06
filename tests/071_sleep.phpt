--TEST--
nocopy: __sleep classes as graph nodes (named slots, defaults in the rest, dynamic properties, __wakeup): same as unserialize(); odd __sleep results serialize with PHP's warnings
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t071
--FILE--
<?php
require __DIR__ . '/serve.inc';
$classes = <<<'CODE'
class Base { private $secret = 'base-default'; function setSecret($v) { $this->secret = $v; } function getSecret() { return $this->secret; } }
#[AllowDynamicProperties]
class Sl extends Base {
    public $pub = 'pub-default'; private $priv = 'priv-default'; protected $prot = 'prot-default';
    public int $typed; public ?int $skipped = 5; public $conn = 'conn-default';
    public $woke = false;
    function set() { $this->pub = 'p'; $this->priv = ['a' => [1, 2]]; $this->prot = str_repeat('x', 40); $this->skipped = 9; $this->conn = 'live'; $this->setSecret('s'); $this->dyn = 'd'; }
    function __sleep() { return ['pub', 'priv', 'prot', 'typed', "\0Base\0secret", 'dyn']; }
    function __wakeup() { $this->woke = true; }
}
class NoWake { public $a = 1; public $b = 2; function __sleep() { return ['a']; } }
class Bad { public $a = 1; function __sleep() { return ['a', 'missing']; } }
class Dup { public $a = 1; function __sleep() { return ['a', 'a']; } }
CODE;
$test = <<<'CODE'
$s = new Sl; $s->set();
$n = new NoWake; $n->a = 'x'; $n->b = 'not stored';
$vals = ['sl' => $s, 'list' => [$n, $n, $s]];
foreach ($vals as $k => $v) {
    nocopy_put($k, $v);
    for ($i = 0; $i < 3; $i++) {
        if (serialize(nocopy_get($k)) !== serialize(unserialize(serialize($v)))) echo "$k #$i DIFFERS\n";
    }
}
$g = nocopy_get('sl');
var_dump($g->woke, $g->conn, $g->skipped, isset($g->typed), $g->getSecret(), $g->dyn);
$l = nocopy_get('list');
var_dump($l[0] === $l[1], $l[0]->b, $l[2]->woke);
$g->pub = 'changed'; $g->dyn = 'changed';
var_dump(nocopy_get('sl')->pub, nocopy_get('sl')->dyn);
CODE;
echo "-- copy mode\n";
eval($classes . $test);
var_dump(nocopy_put('bad', new Bad), nocopy_get('bad')->a);
var_dump(nocopy_put('dup', new Dup), nocopy_get('dup')->a);
echo "-- zero-copy\n";
echo serve('<?php ' . $classes . $test);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t071*')); ?>
--EXPECTF--
-- copy mode
bool(true)
string(12) "conn-default"
int(5)
bool(false)
string(1) "s"
string(1) "d"
bool(true)
int(2)
bool(true)
string(1) "p"
string(1) "d"

Warning: nocopy_put(): "missing" returned as member variable from __sleep() but does not exist in %s on line %d
bool(true)
int(1)

%s: nocopy_put(): "a" is returned from __sleep() multiple times in %s on line %d
bool(true)
int(1)
-- zero-copy
bool(true)
string(12) "conn-default"
int(5)
bool(false)
string(1) "s"
string(1) "d"
bool(true)
int(2)
bool(true)
string(1) "p"
string(1) "d"
