<?php
// php fuzz.php [iterations] [seed] [zero]: random values in, serialize() of what comes back must equal serialize() of what went in.
// A small arena (docker/fuzz.sh: nocopy.shm_size=4M) so merges move graphs and evict. Same iterations + seed: same run. zero: as one php -S request.
$iters = (int) ($argv[1] ?? 20000);
$seed = (int) ($argv[2] ?? random_int(1, PHP_INT_MAX >> 1));
if (($argv[3] ?? '') === 'zero' && PHP_SAPI === 'cli') {
    require __DIR__ . '/serve.inc';
    echo serve('<?php $argv = [0, ' . $iters . ', ' . $seed . ', "zero"]; require ' . var_export(__FILE__, true) . ';', ['nocopy.shm_size' => ini_get('nocopy.shm_size')]);
    exit;
}
error_reporting(E_ALL & ~E_DEPRECATED & ~E_WARNING); // dynamic properties, __sleep() warnings (serialize() gives them too)

class Plain { public $a; public $b = 'def'; }
class Typed { public int $i = 0; public ?string $s = null; public array $arr = []; public ?Typed $next = null; public float $f; }
class RO { public function __construct(public readonly mixed $v) {} }
class Base { private $p = 'base-private'; protected $q = 'base-protected'; public $r; }
class Child extends Base { private $p = 'child-private'; public $c; }
class Sleepy { public $keep; public $drop = 'dropped'; protected $prot; private $priv; public function __sleep(): array { return ['keep', 'prot', 'priv']; } }
class Wake { public $v; public $woke = 0; public function __wakeup(): void { $this->woke++; } }
class SleepWake { public $x; public $y = 'y'; public function __sleep(): array { return ['x']; } public function __wakeup(): void { $this->y = 'woke'; } }
class Ser { public $d; public function __serialize(): array { return ['d' => $this->d, 'n' => 1]; } public function __unserialize(array $a): void { $this->d = $a['d']; } }
#[AllowDynamicProperties] class Dyn { public $fixed = 1; }
enum Pure { case A; case B; }
enum Back: int { case One = 1; case Two = 2; }

const ZONES = ['UTC', 'Europe/Berlin', 'America/New_York', 'Asia/Kathmandu', 'Australia/Lord_Howe', '+05:30', '-00:30', 'CEST', 'EST', 'Z'];

function r(int $n): int { return mt_rand(0, $n - 1); }

function scalar(): mixed
{
    switch (r(14)) {
        case 0: return null;
        case 1: return (bool) r(2);
        case 2: return [0, 1, -1, PHP_INT_MAX, PHP_INT_MIN, 255, 65536][r(7)];
        case 3: return mt_rand(PHP_INT_MIN, PHP_INT_MAX);
        case 4: return [0.0, -0.0, 1.5, NAN, INF, -INF, 1e300, PHP_FLOAT_EPSILON][r(8)];
        case 5: return '';
        case 6: return chr(r(256));
        case 7: return ['a', 'b', 'key', 'id', 'name', '0', '1', '-1', '01', '9223372036854775808'][r(10)];
        case 8: return str_repeat('x', r(80)); // around NOCOPY_VSTR_MAX: deduped or not
        case 9: return implode(array_map(fn() => chr(r(256)), range(0, r(40))));
        case 10: return str_repeat('long', 50 + r(500));
        case 11: return 'v' . r(20);
        case 12: return new DateTimeImmutable('@' . mt_rand(-5000000000, 5000000000) . '.' . r(1000000));
        default: $d = new DateTime('@' . mt_rand(-3000000000, 4000000000)); $d->setTimezone(new DateTimeZone(ZONES[r(count(ZONES))])); return $d;
    }
}

final class Gen
{
    public array $objs = [];
    public array $refs = [];
    private int $budget = 400; // nodes per value: nesting would grow it exponentially

    public function value(int $depth): mixed
    {
        if (--$this->budget < 0 || $depth > 70 || ($depth > 2 && r(3) === 0)) {
            return scalar();
        }
        switch (r(12)) {
            case 0: case 1: case 2: return scalar();
            case 3: case 4: return $this->arr($depth);
            case 5: case 6: case 7: return $this->obj($depth);
            case 8: return $this->objs ? $this->objs[r(count($this->objs))] : scalar(); // the same object again (or a cycle)
            case 9: return r(2) ? Pure::A : Back::cases()[r(2)];
            case 10: return r(40) === 0 ? fn() => 1 : $this->arr($depth); // serialize() throws
            default: return $this->deep(60 + r(10));
        }
    }

    private function deep(int $n): mixed
    {
        $v = scalar();
        for ($i = 0; $i < $n; $i++) $v = [$v];
        return $v;
    }

    private function arr(int $depth): array
    {
        $a = [];
        $n = r(4) === 0 ? r(40) : r(6);
        $packed = r(2);
        for ($i = 0; $i < $n; $i++) {
            $k = $packed ? $i : [r(100), 'k' . r(20), (string) r(5), -r(5), 'name'][r(5)];
            unset($a[$k]); // a reference already there would take the new value
            if (r(8) === 0 && $this->refs) {
                $a[$k] = &$this->refs[r(count($this->refs))]; // a shared reference
            } else {
                $a[$k] = $this->value($depth + 1);
                if (r(10) === 0 && !is_object($a[$k])) { // to an object: serialize() makes R: of a ref held once, nocopy keeps it plain
                    $this->refs[] = &$a[$k];
                }
            }
        }
        if ($packed && $n > 2 && r(4) === 0) unset($a[r($n)]); // a hole
        return $a;
    }

    private function obj(int $depth): object
    {
        $v = fn() => $this->value($depth + 1);
        switch (r(13)) {
            case 0: $o = new Plain; $this->objs[] = $o; $o->a = $v(); if (r(2)) $o->b = $v(); return $o;
            case 1: $o = new Typed; $this->objs[] = $o; $o->i = r(1000); $o->s = r(2) ? null : 's' . r(9); $o->arr = $this->arr($depth + 1);
                if (r(2)) $o->f = 1.25; if (r(3) === 0) { $o->next = $o; } elseif (r(3) === 0) { $o->next = new Typed; } return $o;
            case 2: $o = new RO($v()); $this->objs[] = $o; return $o;
            case 3: $o = new Child; $this->objs[] = $o; $o->r = $v(); $o->c = $v(); return $o;
            case 4: $o = new Sleepy; $this->objs[] = $o; $o->keep = $v(); return $o;
            case 5: $o = new Wake; $this->objs[] = $o; $o->v = $v(); return $o;
            case 6: $o = new SleepWake; $this->objs[] = $o; $o->x = $v(); return $o;
            case 7: $o = new Ser; $this->objs[] = $o; $o->d = $v(); return $o;
            case 8: $o = new stdClass; $this->objs[] = $o; for ($i = r(5); $i > 0; $i--) $o->{r(2) ? 'p' . r(9) : r(9)} = $v(); return $o;
            case 9: $o = new Dyn; $this->objs[] = $o; $o->{'d' . r(5)} = $v(); return $o;
            case 10: $o = new ArrayObject([$v(), 'k' => $v()]); $this->objs[] = $o; return $o;
            case 11: $o = new SplObjectStorage; $this->objs[] = $o; $o[new Plain] = $v(); return $o;
            default: $o = r(2) ? new DateTime('2026-03-29 02:30:00', new DateTimeZone(ZONES[r(count(ZONES))])) : new DateTimeImmutable('@' . r(2000000000));
                if (r(20) === 0) $o->extra = 1; return $o; // dynamic property: serialized whole
        }
    }
}

function ser(mixed $v): ?string
{
    try {
        return serialize($v);
    } catch (Throwable) {
        return null;
    }
}

// What a fetch should look like: serialize() after an unserialize() (which runs __wakeup/__unserialize).
function canon(?string $s): ?string
{
    return $s === null ? null : serialize(unserialize($s));
}

if (($only = getenv('FUZZ_VALUE')) !== false) { // the value one iteration generated, as var_dump() shows it
    mt_srand($seed + (int) $only);
    r(1024);
    r(10);
    var_dump((new Gen)->value(0));
    exit;
}
$want = $from = []; // key => serialize() of what is stored there, the iteration that stored it
$bad = 0;
for ($i = 0; $i < $iters && $bad < 5; $i++) {
    mt_srand($seed + $i);
    $key = 'k' . r(1024);
    $op = r(10);
    if ($op < 6) {
        $g = new Gen;
        $v = $g->value(0);
        $s = canon(ser($v));
        try {
            $ok = nocopy_put($key, $v);
        } catch (Throwable $e) {
            $ok = null;
        }
        if ($ok === null && $s !== null) {
            echo "#$i put threw but serialize() did not: ", get_class($e), ': ', $e->getMessage(), "\n";
            $bad++;
        }
        if ($ok) [$want[$key], $from[$key]] = [$s, $i]; // a failed put leaves what was there
        unset($g, $v);
    } elseif ($op < 9) {
        for ($n = 0; $n < 3; $n++) { // copy mode keeps a template from the second fetch on
            try {
                $got = nocopy_get($key, $hit);
            } catch (Throwable $e) {
                echo "#$i get threw: ", get_class($e), ': ', $e->getMessage(), "\n";
                $bad++;
                break;
            }
            if (!$hit) {
                unset($want[$key]); // evicted
                break;
            }
            if (!isset($want[$key])) {
                echo "#$i hit on a key never stored or deleted\n";
                $bad++;
                break;
            }
            $gs = ser($got);
            if ($gs !== $want[$key]) {
                echo "#$i $key (stored by #{$from[$key]}: FUZZ_VALUE=$from[$key]) DIFFERS at byte ", $at = strspn($gs ^ $want[$key], "\0"), "\n  got:  ", substr((string) $gs, max(0, $at - 150), 300), "\n  want: ", substr($want[$key], max(0, $at - 150), 300), "\n";
                $bad++;
                break;
            }
            unset($got);
        }
    } else {
        nocopy_delete($key);
        unset($want[$key]);
    }
}
$st = nocopy_stats();
printf("%s: %d iterations (evicted %d, %d KB moved by merges), seed %d, %s\n", PHP_SAPI === 'cli' ? 'copy' : 'zero', $i, $st['evicted'], $st['evacuated_bytes'] >> 10, $seed, $bad ? "$bad FAILED" : 'ok');
