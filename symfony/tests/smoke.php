<?php
// php -d extension=stash.so -d apc.enable_cli=1 tests/smoke.php <dir with vendor/autoload.php>

use Lab\StashBundle\StashBundle;
use Symfony\Bundle\FrameworkBundle\FrameworkBundle;
use Symfony\Bundle\FrameworkBundle\Kernel\MicroKernelTrait;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;
use Symfony\Component\HttpKernel\Kernel;

require ($argv[1] ?? __DIR__.'/..').'/vendor/autoload.php';
require __DIR__.'/../src/StashBundle.php';

class SmokeKernel extends Kernel
{
    use MicroKernelTrait;

    public function registerBundles(): iterable
    {
        yield new FrameworkBundle();
        yield new StashBundle();
    }

    public function getCacheDir(): string { static $d; return $d ??= sys_get_temp_dir().'/stash-smoke/'.uniqid(); }

    public function getLogDir(): string { return sys_get_temp_dir().'/stash-smoke/log'; }

    protected function configureContainer(ContainerConfigurator $c): void
    {
        $c->extension('framework', ['secret' => 'x', 'cache' => [
            'app' => 'cache.adapter.stash',
            'pools' => ['my.pool' => ['adapter' => 'cache.adapter.stash', 'public' => true, 'default_lifetime' => 60]],
        ]]);
    }
}

$k = new SmokeKernel('prod', false);
$k->boot();
$c = $k->getContainer();

foreach (['cache.app', 'my.pool'] as $id) {
    $pool = $c->get($id);
    $pool->save($pool->getItem('k')->set(['rows' => [1, 2, 3]]));
    assert($pool->getItem('k')->get() === ['rows' => [1, 2, 3]], "$id roundtrip");
}

// stored as arrays, not serialized strings
$raw = array_values(array_filter(iterator_to_array(new APCUIterator('/k$/')), fn ($e) => str_ends_with($e['key'], 'k')));
assert(count($raw) === 2 && is_array($raw[0]['value']) && is_array($raw[1]['value']), 'unmarshalled values');
echo "ok\n";
