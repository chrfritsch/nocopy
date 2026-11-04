<?php
namespace App;

use Symfony\Bundle\FrameworkBundle\FrameworkBundle;
use Symfony\Bundle\FrameworkBundle\Kernel\MicroKernelTrait;
use Lab\NocopyBundle\NocopyBundle;
use Symfony\Component\Cache\Adapter\ApcuAdapter;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;
use Symfony\Component\HttpKernel\Kernel as BaseKernel;
use Symfony\Component\Routing\Loader\Configurator\RoutingConfigurator;

class Kernel extends BaseKernel
{
    use MicroKernelTrait;

    public function registerBundles(): iterable
    {
        yield new FrameworkBundle();
        yield new NocopyBundle();
    }

    protected function configureContainer(ContainerConfigurator $c): void
    {
        $c->extension('framework', [
            'secret' => 'x',
            'http_method_override' => false,
            'handle_all_throwables' => true,
            'php_errors' => ['log' => false],
            'serializer' => ['enabled' => true],
            'validation' => ['enabled' => true],
            'property_info' => ['enabled' => true],
            'cache' => ['app' => 'cache.adapter.apcu', 'prefix_seed' => 'bench'],
        ]);
        // a pool without Symfony's marshaller: the bundle's under nocopy, a plain ApcuAdapter under APCu
        if (extension_loaded('nocopy')) {
            $c->extension('framework', ['cache' => ['pools' => ['app.cache_raw' => ['adapter' => 'cache.adapter.nocopy', 'public' => true]]]]);
        } else {
            $c->services()->set('app.cache_raw', ApcuAdapter::class)->args(['raw', 0, null, null])->public();
        }
        $c->services()->defaults()->autowire()->autoconfigure()->public()
            ->load('App\\', __DIR__)->exclude(__DIR__.'/Kernel.php');
    }

    protected function configureRoutes(RoutingConfigurator $r): void
    {
        $r->import(__DIR__.'/Controller/', 'attribute');
    }
}
