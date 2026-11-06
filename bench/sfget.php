<?php
// php -d extension=nocopy.so -d nocopy.shm_name=/nocopy-sfget bench/sfget.php <dir with vendor/autoload.php>: ns per Symfony get(), bundle vs AbstractAdapter's

use Chrfritsch\NocopyBundle\NocopyAdapter;
use Symfony\Component\Cache\Adapter\AbstractAdapter;

require $argv[1].'/vendor/autoload.php';
require __DIR__.'/../symfony/src/NocopyAdapter.php';

final class SymfonyGet extends NocopyAdapter
{
    public function get(string $key, callable $callback, ?float $beta = null, ?array &$metadata = null): mixed
    {
        return AbstractAdapter::get($key, $callback, $beta, $metadata);
    }
}

$value = ['id' => 1, 'title' => str_repeat('x', 100), 'tags' => range(1, 20)];
$ttl = fn ($item) => [$item->expiresAfter(3600), $value][1];
$none = fn () => $value;
$n = 200000;
foreach (['nocopy' => new NocopyAdapter('b'), 'symfony' => new SymfonyGet('b')] as $name => $pool) {
    foreach (['hit, expiresAfter' => $ttl, 'hit, no expiry' => $none] as $case => $f) {
        $pool->clear();
        $pool->get('k', $f);
        $best = INF;
        for ($r = 0; $r < 5; $r++) {
            $t = hrtime(true);
            for ($i = 0; $i < $n; $i++) {
                $pool->get('k', $f);
            }
            $best = min($best, (hrtime(true) - $t) / $n);
        }
        printf("%-8s %-18s %6.0f ns\n", $name, $case, $best);
    }
    $pool->clear();
    $t = hrtime(true);
    for ($i = 0; $i < 50000; $i++) {
        $pool->get("m$i", $ttl);
    }
    printf("%-8s %-18s %6.0f ns\n", $name, 'miss + store', (hrtime(true) - $t) / 50000);
}
