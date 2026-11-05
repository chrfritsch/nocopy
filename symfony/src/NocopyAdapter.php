<?php

namespace Chrfritsch\NocopyBundle;

use Symfony\Component\Cache\Adapter\AbstractAdapter;
use Symfony\Component\Cache\Exception\CacheException;

/** Symfony cache pool on nocopy_*: needs no APCu layer, works next to real APCu. */
class NocopyAdapter extends AbstractAdapter
{
    // Valkey has no eviction of its own here: cap "forever" so versioned-out keys leave
    private const PERM_TTL = 31536000;

    private readonly bool $valkey;

    public function __construct(string $namespace = '', int $defaultLifetime = 0, ?string $version = null)
    {
        $with = \function_exists('nocopy_shared_with') ? nocopy_shared_with() : 'none (the nocopy extension is not loaded)';
        if ('host' !== $with && 'valkey' !== $with) {
            // a process with a cache of its own would serve what other processes already changed
            throw new CacheException(\sprintf('nocopy: this %s process does not share the cache (nocopy_shared_with(): %s). Set nocopy.shm_name (one server) or nocopy.valkey (several) in the php.ini of every PHP that runs this app, and run the console as the user that starts php-fpm.', \PHP_SAPI, $with));
        }
        $this->valkey = 'valkey' === $with;
        parent::__construct($namespace, $defaultLifetime);
        // ponytail: clear() bumps a namespace version instead of deleting keys; eviction (shm) or PERM_TTL (Valkey) drops the old ones
        $this->enableVersioning();

        if (null !== $version) {
            $marker = $version.'@'.$namespace;
            nocopy_get($marker, $hit);
            // clear before marking: marking first would let another process read the old build's version in between
            if (!$hit) {
                $this->clear();
                nocopy_put($marker, true, $this->valkey ? self::PERM_TTL : 0);
            }
        }
    }

    protected function doFetch(array $ids): iterable
    {
        $handler = ini_set('unserialize_callback_func', __CLASS__.'::handleUnserializeCallback');
        try {
            return nocopy_get_multiple($ids);
        } catch (\Error $e) {
            throw new \ErrorException($e->getMessage(), $e->getCode(), \E_ERROR, $e->getFile(), $e->getLine());
        } finally {
            ini_set('unserialize_callback_func', $handler);
        }
    }

    protected function doHave(string $id): bool
    {
        nocopy_get($id, $hit);

        return $hit;
    }

    protected function doClear(string $namespace): bool
    {
        return false; // versioning did the work; there is no prefix delete
    }

    protected function doDelete(array $ids): bool
    {
        nocopy_delete_multiple($ids);

        return true;
    }

    // false: AbstractAdapter retries key by key and logs the failures
    protected function doSave(array $values, int $lifetime): array|bool
    {
        return nocopy_put_multiple($values, 0 === $lifetime && $this->valkey ? self::PERM_TTL : $lifetime, true);
    }
}
