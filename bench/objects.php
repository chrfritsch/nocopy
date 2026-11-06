<?php
// php [-d nocopy.*] objects.php [zero]: object fetches, nocopy vs unserialize() (and deepclone's payload if loaded). zero: as one php -S request. Needs NOCOPY_SO for zero.
if (($argv[1] ?? '') === 'zero' && PHP_SAPI === 'cli') {
    require __DIR__ . '/../tests/serve.inc';
    echo serve('<?php require ' . var_export(__FILE__, true) . ';', array_filter(['extension' => extension_loaded('deepclone') ? 'deepclone' : null]));
    exit;
}
final class Category { public function __construct(public int $id, public string $name) {} }
class Product {
    private array $meta = ['source' => 'import', 'flags' => [1, 2, 3]];
    public function __construct(public int $id, public string $name, protected string $desc, public array $tags, public ?Category $cat, public float $price) {}
}
$cats = [new Category(1, 'Books'), new Category(2, 'Music'), new Category(3, 'Games')];
$mk = fn($i) => new Product($i, "Product $i", str_repeat("Description of product $i. ", 4), ['a', 'b', "t$i"], $cats[$i % 3], $i * 1.5);
$row = fn($i) => ['id' => $i, 'name' => "Product $i", 'desc' => str_repeat("Description of product $i. ", 4), 'tags' => ['a', 'b', "t$i"]];
$cases = [
    'one object' => $mk(1),
    '100 objects' => array_map($mk, range(1, 100)),
    '100 rows + 1 object' => ['rows' => array_map($row, range(1, 100)), 'cat' => $cats[0]],
];
$dc = function_exists('deepclone_to_array');
function bench(callable $f, int $n): float { $f(); $t = hrtime(true); for ($i = 0; $i < $n; $i++) $f(); return (hrtime(true) - $t) / $n; }
printf("%-20s %10s %10s %12s\n", PHP_SAPI, 'nocopy', 'deepclone', 'unserialize');
foreach ($cases as $name => $v) {
    $n = is_array($v) ? 20000 : 200000;
    nocopy_put('o', $v);
    $dc && nocopy_put('d', deepclone_to_array($v));
    $s = serialize($v);
    printf("%-20s %8.0fns %10s %10.0fns\n", $name, bench(fn() => nocopy_get('o'), $n),
        $dc ? sprintf('%8.0fns', bench(fn() => deepclone_from_array(nocopy_get('d')), $n)) : '-', bench(fn() => unserialize($s), $n));
}
