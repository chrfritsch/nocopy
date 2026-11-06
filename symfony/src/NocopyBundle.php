<?php

namespace Chrfritsch\NocopyBundle;

use Symfony\Component\Cache\Adapter\AdapterInterface;
use Symfony\Component\DependencyInjection\ContainerBuilder;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;
use Symfony\Component\DependencyInjection\Parameter;
use Symfony\Component\HttpKernel\Bundle\AbstractBundle;

use function Symfony\Component\DependencyInjection\Loader\Configurator\param;
use function Symfony\Component\DependencyInjection\Loader\Configurator\service;

final class NocopyBundle extends AbstractBundle
{
    public function prependExtension(ContainerConfigurator $container, ContainerBuilder $builder): void
    {
        // Symfony puts only APCu in front of its system pools (validator, serializer, ...): nocopy takes that place
        $builder->prependExtensionConfig('framework', ['cache' => ['system' => 'cache.adapter.nocopy_system']]);
    }

    public function loadExtension(array $config, ContainerConfigurator $container, ContainerBuilder $builder): void
    {
        // values reach nocopy as PHP values (zero-copy fetches); the pool empties on each new container build, like APCu's
        $container->services()->set('cache.adapter.nocopy', NocopyAdapter::class)
            ->abstract()
            ->args(['', 0, new Parameter('container.build_id')])
            ->call('setLogger', [service('logger')->ignoreOnInvalid()])
            ->tag('cache.pool', ['clearer' => 'cache.default_clearer', 'reset' => 'reset'])
            ->tag('monolog.logger', ['channel' => 'cache']);
        $container->services()->set('cache.adapter.nocopy_system', AdapterInterface::class)
            ->abstract()
            ->factory([NocopyAdapter::class, 'createSystemCache'])
            ->args(['', 0, new Parameter('container.build_id'), param('kernel.cache_dir').'/pools/system', service('logger')->ignoreOnInvalid()])
            ->tag('cache.pool', ['clearer' => 'cache.system_clearer', 'reset' => 'reset'])
            ->tag('monolog.logger', ['channel' => 'cache']);
    }
}
