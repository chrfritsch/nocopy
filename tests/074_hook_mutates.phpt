--TEST--
nocopy: a __serialize/__sleep that changes the part of the value already walked stores what the hooks left
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t074
--FILE--
<?php
class A { public $p; }
class P { public $v = 'old'; }
class D {
    public $x;
    public function __construct($x) { $this->x = $x; }
    public function __serialize(): array { return ['x' => $this->x]; }
    public function __unserialize(array $d): void { $this->x = $d['x']; }
}
class B {
    public function __construct(public A $a) {}
    public function __serialize(): array { $this->a->p = new D(str_repeat('y', 1000)); return []; }
    public function __unserialize(array $d): void {}
}
class S {
    public $keep = 'k';
    public $drop = 'd';
    public function __construct(public A $a) {}
    public function __sleep(): array { $this->a->p = new P; $this->a->p->v = 'new'; return ['keep', 'a']; }
}

$a = new A;
$a->p = $old = new P;
var_dump(nocopy_put('k', [$a, new B($a)]));
$g = nocopy_get('k');
var_dump(get_class($g[0]->p), strlen($g[0]->p->x));

$a = new A;
$a->p = new D('first');
var_dump(nocopy_put('s', [$a, new S($a)]));
$g = nocopy_get('s');
var_dump(get_class($g[0]->p), $g[0]->p->v, $g[1]->a === $g[0]);

class L { public $list; }
class Grow {
    public function __construct(public L $l) {}
    public function __serialize(): array { for ($i = 0; $i < 100; $i++) { $this->l->list[] = $i; } return []; }
    public function __unserialize(array $d): void {}
}
$l = new L;
$l->list = [new Grow($l), 'x'];
var_dump(nocopy_put('g', $l), count(nocopy_get('g')->list));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t074*')); ?>
--EXPECT--
bool(true)
string(1) "D"
int(1000)
bool(true)
string(1) "P"
string(3) "new"
bool(true)
bool(true)
int(102)
