<?php
// NocopyBackend's read path against the bare lookup, ns per call (best of 5). CLI, so copy mode: values are small.
//   php get.php <drupal root> <module dir>
[, $drupal, $module] = $argv;
$loader = require "$drupal/vendor/autoload.php";
$loader->addPsr4('Drupal\\nocopy\\', "$module/src");

use Drupal\Component\Datetime\Time;
use Drupal\Core\Site\Settings;
use Drupal\nocopy\Cache\NocopyBackend;
use Drupal\nocopy\Cache\NocopyCacheTagsChecksum;

new Settings(['hash_salt' => 'bench']);
$bin = new NocopyBackend('render', 'bench', new NocopyCacheTagsChecksum('/', 'bench'), new Time());

$tags = fn (int $n) => array_map(fn ($i) => "node:$i", range(1, $n));
$data = ['#markup' => str_repeat('x', 200), '#attached' => ['library' => ['core/drupal']], '#cache' => ['max-age' => -1]];
foreach ([0, 3, 20] as $n) {
    $bin->set("t$n", $data, -1, $n ? $tags($n) : []);
}
$multi = [];
for ($i = 0; $i < 10; $i++) {
    $bin->set($multi[] = "m$i", $data, -1, $tags(3));
}
$bin->set('expiring', $data, time() + 3600, $tags(3));

$bench = function (string $name, callable $f, int $per = 1) {
    $best = INF;
    for ($r = 0; $r < 5; $r++) {
        $t = hrtime(true);
        for ($i = 0; $i < 20000; $i++) {
            $f();
        }
        $best = min($best, (hrtime(true) - $t) / 20000 / $per);
    }
    printf("%-28s %6.0f ns\n", $name, $best);
};
$key = $bin->versionKey();
$bench('nocopy_get (bare)', fn () => nocopy_get($key));
$bench('get miss', fn () => $bin->get('nope'));
$bench('get hit, no tags', fn () => $bin->get('t0'));
$bench('get hit, 3 tags', fn () => $bin->get('t3'));
$bench('get hit, 20 tags', fn () => $bin->get('t20'));
$bench('get hit, 3 tags, expiring', fn () => $bin->get('expiring'));
$bench('getMultiple 10, per item', function () use ($bin, $multi) { $c = $multi; $bin->getMultiple($c); }, 10);
$bench('getMultiple 10 misses, per item', function () use ($bin) { $c = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j']; $bin->getMultiple($c); }, 10);
