<?php
// php -d extension=nocopy.so -d zend.assertions=1 tests/smoke.php <dir with vendor/autoload.php>

use Chrfritsch\NocopyBundle\NocopyBundle;
use Symfony\Bundle\FrameworkBundle\FrameworkBundle;
use Symfony\Bundle\FrameworkBundle\Kernel\MicroKernelTrait;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;
use Symfony\Component\HttpKernel\Kernel;

require ($argv[1] ?? __DIR__.'/..').'/vendor/autoload.php';
require __DIR__.'/../src/NocopyBundle.php';
require __DIR__.'/../src/NocopyAdapter.php';

class SmokeKernel extends Kernel
{
    use MicroKernelTrait;

    public function registerBundles(): iterable
    {
        yield new FrameworkBundle();
        yield new NocopyBundle();
    }

    public function getCacheDir(): string { static $d; return $d ??= sys_get_temp_dir().'/nocopy-smoke/'.uniqid(); }

    public function getLogDir(): string { return sys_get_temp_dir().'/nocopy-smoke/log'; }

    protected function configureContainer(ContainerConfigurator $c): void
    {
        $c->extension('framework', ['secret' => 'x', 'cache' => [
            'app' => 'cache.adapter.nocopy',
            'pools' => ['my.pool' => ['adapter' => 'cache.adapter.nocopy', 'public' => true, 'default_lifetime' => 60]],
        ]]);
    }
}

if ('none' === nocopy_shared_with()) {
    try {
        new Chrfritsch\NocopyBundle\NocopyAdapter();
        assert(false, 'unshared cache must throw');
    } catch (Symfony\Component\Cache\Exception\CacheException) {
    }
    exit("ok (not shared: refused)\n");
}

$k = new SmokeKernel('prod', false);
$k->boot();
$c = $k->getContainer();

$sys = $c->get('cache.system');
$first = (new ReflectionProperty(Symfony\Component\Cache\Adapter\ChainAdapter::class, 'adapters'))->getValue($sys)[0];
assert($first instanceof Chrfritsch\NocopyBundle\NocopyAdapter, 'system pools: nocopy in front of PHP files');
$sys->save($sys->getItem('s')->set(1));
assert($first->getItem('s')->isHit(), 'system pool roundtrip');

$id = new ReflectionMethod(Symfony\Component\Cache\Adapter\AbstractAdapter::class, 'getId');
foreach (['cache.app', 'my.pool'] as $name) {
    $pool = $c->get($name);
    assert($pool instanceof Chrfritsch\NocopyBundle\NocopyAdapter, "$name adapter");
    $pool->save($pool->getItem('k')->set(['rows' => [1, 2, 3]]));
    $pool->save($pool->getItem('o')->set(new ArrayObject([1])));
    assert($pool->getItem('k')->get() === ['rows' => [1, 2, 3]], "$name roundtrip");
    assert($pool->getItem('o')->get() == new ArrayObject([1]), "$name object");
    assert(array_map(fn ($i) => $i->isHit(), iterator_to_array($pool->getItems(['k', 'o', 'nope']))) === ['k' => true, 'o' => true, 'nope' => false], "$name getItems");
    assert(is_array(nocopy_get($id->invoke($pool, 'k'))), "$name stores PHP values, not serialized strings");
    assert($pool->deleteItem('o') && !$pool->hasItem('o') && $pool->hasItem('k'), "$name delete");
    assert($pool->clear() && !$pool->hasItem('k'), "$name clear");
    $pool->reset(); // next request: the new namespace version is read back from nocopy
    assert(!$pool->getItem('k')->isHit(), "$name clear survives reset");

    $n = 0;
    $f = function ($item) use (&$n) { $item->expiresAfter(100); return ['n' => ++$n]; };
    assert($pool->get('g', $f) === ['n' => 1] && $pool->get('g', $f) === ['n' => 1], "$name get computes once");
    assert(nocopy_get($id->invoke($pool, 'g')) === ['n' => 1], "$name get stores the value unwrapped");
    assert($pool->getItem('g')->isHit() && $pool->get('g', $f, INF) === ['n' => 2], "$name get: getItem sees it, INF recomputes");
    $pool->get('ns', function ($item, &$save) { $save = false; return 1; });
    $pool->get('old', fn ($item) => $item->expiresAt(new DateTime('-1 sec')) ? 1 : 0);
    assert(!$pool->hasItem('ns') && !$pool->hasItem('old'), "$name get: \$save = false and past expiry keep nothing");
    $chain = new Symfony\Component\Cache\Adapter\ChainAdapter([$pool, $arr = new Symfony\Component\Cache\Adapter\ArrayAdapter()]);
    assert($chain->get('c', fn () => 'v') === 'v' && $pool->getItem('c')->isHit() && $arr->getItem('c')->isHit(), "$name get in a chain");
}
echo "ok\n";
