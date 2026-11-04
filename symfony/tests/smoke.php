<?php
// php -d extension=nocopy.so -d zend.assertions=1 tests/smoke.php <dir with vendor/autoload.php>  (also with nocopy.apcu_compat=0)

use Lab\NocopyBundle\NocopyBundle;
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

$k = new SmokeKernel('prod', false);
$k->boot();
$c = $k->getContainer();

$id = new ReflectionMethod(Symfony\Component\Cache\Adapter\AbstractAdapter::class, 'getId');
foreach (['cache.app', 'my.pool'] as $name) {
    $pool = $c->get($name);
    assert($pool instanceof Lab\NocopyBundle\NocopyAdapter, "$name adapter");
    $pool->save($pool->getItem('k')->set(['rows' => [1, 2, 3]]));
    $pool->save($pool->getItem('o')->set(new ArrayObject([1])));
    assert($pool->getItem('k')->get() === ['rows' => [1, 2, 3]], "$name roundtrip");
    assert($pool->getItem('o')->get() == new ArrayObject([1]), "$name object");
    assert(is_array(nocopy_get($id->invoke($pool, 'k'))), "$name stores PHP values, not serialized strings");
    assert($pool->deleteItem('o') && !$pool->hasItem('o') && $pool->hasItem('k'), "$name delete");
    assert($pool->clear() && !$pool->hasItem('k'), "$name clear");
    $pool->reset(); // next request: the new namespace version is read back from nocopy
    assert(!$pool->getItem('k')->isHit(), "$name clear survives reset");
}
echo "ok\n";
