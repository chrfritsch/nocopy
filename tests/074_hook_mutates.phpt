--TEST--
nocopy: a __serialize/__sleep that changes the value while it is walked stores what serialize() would
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

class L { public $list; }
class Grow {
    public function __construct(public L $l) {}
    public function __serialize(): array { for ($i = 0; $i < 100; $i++) { $this->l->list[] = $i; } return []; }
    public function __unserialize(array $d): void {}
}

// each case built twice: once through nocopy, once through serialize()
$cases = [
    'k' => function () { $a = new A; $a->p = new P; return [$a, new B($a)]; },
    's' => function () { $a = new A; $a->p = new D('first'); return [$a, new S($a)]; },
    'g' => function () { $l = new L; $l->list = [new Grow($l), 'x']; return $l; },
];
$show = fn($k, $g) => match ($k) {
    'k' => [get_class($g[0]->p), isset($g[0]->p->x) ? strlen($g[0]->p->x) : null],
    's' => [get_class($g[0]->p), $g[0]->p->x ?? $g[0]->p->v, $g[1]->a === $g[0]],
    'g' => [count($g->list)],
};
foreach ($cases as $k => $make) {
    var_dump(nocopy_put($k, $make()));
    $mine = $show($k, nocopy_get($k));
    $ser = $show($k, unserialize(serialize($make())));
    echo $k, ': ', json_encode($mine), $mine === $ser ? ' same as serialize()' : ' serialize(): ' . json_encode($ser), "\n";
}
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t074*')); ?>
--EXPECT--
bool(true)
k: ["P",null] same as serialize()
bool(true)
s: ["D","first",true] same as serialize()
bool(true)
g: [2] same as serialize()
