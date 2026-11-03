<?php

namespace Lab\StashBundle;

use Symfony\Component\DependencyInjection\ContainerBuilder;
use Symfony\Component\DependencyInjection\Loader\Configurator\ContainerConfigurator;
use Symfony\Component\HttpKernel\Bundle\AbstractBundle;

final class StashBundle extends AbstractBundle
{
    public function loadExtension(array $config, ContainerConfigurator $container, ContainerBuilder $builder): void
    {
        // cache.adapter.apcu without Symfony's marshaller: values reach stash as PHP values (zero-copy fetches)
        $container->services()->set('cache.adapter.stash')
            ->parent('cache.adapter.apcu')
            ->abstract()
            ->arg('$marshaller', null);
    }
}
