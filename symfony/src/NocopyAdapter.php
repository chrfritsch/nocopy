<?php

namespace Chrfritsch\NocopyBundle;

use Psr\Log\LoggerInterface;
use Symfony\Component\Cache\Adapter\AbstractAdapter;
use Symfony\Component\Cache\Adapter\AdapterInterface;
use Symfony\Component\Cache\Adapter\ChainAdapter;
use Symfony\Component\Cache\Adapter\PhpFilesAdapter;
use Symfony\Component\Cache\CacheItem;
use Symfony\Component\Cache\Exception\CacheException;
use Symfony\Component\Cache\Exception\InvalidArgumentException;

/** Symfony cache pool on nocopy_*: needs no APCu layer, works next to real APCu. */
class NocopyAdapter extends AbstractAdapter
{
    // Valkey has no eviction of its own here: cap "forever" so versioned-out keys leave
    private const PERM_TTL = 31536000;

    private static \Closure $compute;

    private readonly bool $valkey;
    private readonly int $lifetime;

    public function __construct(string $namespace = '', int $defaultLifetime = 0, ?string $version = null)
    {
        $with = self::sharedWith();
        if (null === $with) {
            $with = \function_exists('nocopy_shared_with') ? nocopy_shared_with() : 'none (the nocopy extension is not loaded)';
            // a process with a cache of its own would serve what other processes already changed
            throw new CacheException(\sprintf('nocopy: this %s process does not share the cache (nocopy_shared_with(): %s). Set nocopy.shm_name (one server) or nocopy.valkey (several) in the php.ini of every PHP that runs this app, and run the console as the user that starts php-fpm.', \PHP_SAPI, $with));
        }
        $this->valkey = 'valkey' === $with;
        $this->lifetime = $defaultLifetime;
        self::$compute ??= \Closure::bind(static function (string $key, callable $callback, bool &$save): array {
            $item = new CacheItem();
            $item->key = $key;

            return [$callback($item, $save), $item->expiry];
        }, null, CacheItem::class);
        parent::__construct($namespace, $defaultLifetime);
        // ponytail: clear() bumps a namespace version instead of deleting keys; eviction (shm) or PERM_TTL (Valkey) drops the old ones
        $this->enableVersioning();

        if (null !== $version) {
            $marker = $version.'@'.$namespace;
            nocopy_get($marker, $hit);
            // clear before marking: marking first would let another process read the old build's version in between
            if (!$hit) {
                $this->clear();
                nocopy_put($marker, true, $this->ttl(0));
            }
        }
    }

    /** AbstractAdapter::createSystemCache() with nocopy in APCu's place; Symfony's own where this process does not share nocopy. */
    public static function createSystemCache(string $namespace, int $defaultLifetime, string $version, string $directory, ?LoggerInterface $logger = null): AdapterInterface
    {
        if (null === self::sharedWith()) {
            return parent::createSystemCache($namespace, $defaultLifetime, $version, $directory, $logger);
        }
        $files = new PhpFilesAdapter($namespace, $defaultLifetime, $directory, true);
        $nocopy = new self($namespace, intdiv($defaultLifetime, 5), $version);
        if (null !== $logger) {
            $files->setLogger($logger);
            $nocopy->setLogger($logger);
        }

        return new ChainAdapter([$nocopy, $files]);
    }

    /** Single flight, stale-while-revalidate and early refresh in nocopy_remember, instead of Symfony's file locks and value wrappers. */
    public function get(string $key, callable $callback, ?float $beta = null, ?array &$metadata = null): mixed
    {
        if (0 > $beta ??= 1.0) {
            throw new InvalidArgumentException(\sprintf('Argument "$beta" provided to "%s::get()" must be a positive number, %f given.', static::class, $beta));
        }
        $id = $this->getId($key);
        if (\INF === $beta) {
            nocopy_delete($id);
        }
        $metadata = []; // ponytail: no expiry/ctime returned; ChainAdapter fills them from the pools below
        // ponytail: ignores a pending saveDeferred() of the same key and setCallbackWrapper()

        return nocopy_remember($id, function (string $id, int &$ttl) use ($key, $callback): mixed {
            $save = true;
            [$value, $expiry] = (self::$compute)($key, $callback, $save);
            if (!$save) {
                $ttl = -1;
            } elseif (null !== $expiry) {
                $ttl = $expiry ? (int) (0.1 + $expiry - microtime(true)) : 0;
                if ($expiry && 0 >= $ttl) {
                    $ttl = -1; // already expired: return it, keep nothing
                }
            }
            $ttl = $this->ttl($ttl);

            return $value;
        }, $this->lifetime, $beta);
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
        return nocopy_put_multiple($values, $this->ttl($lifetime));
    }

    /** 'host' or 'valkey'; null: this process does not share a nocopy cache. */
    private static function sharedWith(): ?string
    {
        $with = \function_exists('nocopy_shared_with') ? nocopy_shared_with() : 'none';

        return 'host' === $with || 'valkey' === $with ? $with : null;
    }

    /** A ttl as stored: 0 (forever) is PERM_TTL under Valkey. */
    private function ttl(int $ttl): int
    {
        return 0 === $ttl && $this->valkey ? self::PERM_TTL : $ttl;
    }
}
