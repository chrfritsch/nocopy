<?php

namespace Chrfritsch\NocopyBundle;

use Symfony\Component\DependencyInjection\ContainerBuilder;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;
use Symfony\Component\DependencyInjection\Parameter;
use Symfony\Component\HttpKernel\Bundle\AbstractBundle;

use function Symfony\Component\DependencyInjection\Loader\Configurator\service;

final class NocopyBundle extends AbstractBundle
{
    public function loadExtension(array $config, ContainerConfigurator $container, ContainerBuilder $builder): void
    {
        // values reach nocopy as PHP values (zero-copy fetches); the pool empties on each new container build, like APCu's
        $container->services()->set('cache.adapter.nocopy', NocopyAdapter::class)
            ->abstract()
            ->args(['', 0, new Parameter('container.build_id')])
            ->call('setLogger', [service('logger')->ignoreOnInvalid()])
            ->tag('cache.pool', ['clearer' => 'cache.default_clearer', 'reset' => 'reset'])
            ->tag('monolog.logger', ['channel' => 'cache']);
    }
}
