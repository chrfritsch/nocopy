<?php

namespace Lab\StashBundle;

use Symfony\Component\Cache\Adapter\AbstractAdapter;
use Symfony\Component\Cache\Exception\CacheException;

/** Symfony cache pool on stash_*: works with stash.apcu_compat=0 and next to real APCu. */
class StashAdapter extends AbstractAdapter
{
    public function __construct(string $namespace = '', int $defaultLifetime = 0, ?string $version = null)
    {
        if (!\function_exists('stash_get')) {
            throw new CacheException('The stash extension is not loaded.');
        }
        parent::__construct($namespace, $defaultLifetime);
        // ponytail: clear() bumps a namespace version instead of deleting keys; stash's eviction drops the unread old ones
        $this->enableVersioning();

        if (null !== $version) {
            $marker = $version.'@'.$namespace;
            stash_get($marker, $hit);
            if (!$hit) {
                $this->clear();
                stash_put($marker, true);
            }
        }
    }

    protected function doFetch(array $ids): iterable
    {
        $handler = ini_set('unserialize_callback_func', __CLASS__.'::handleUnserializeCallback');
        try {
            $values = [];
            foreach ($ids as $id) {
                $v = stash_get($id, $hit);
                if ($hit) {
                    $values[$id] = $v;
                }
            }

            return $values;
        } catch (\Error $e) {
            throw new \ErrorException($e->getMessage(), $e->getCode(), \E_ERROR, $e->getFile(), $e->getLine());
        } finally {
            ini_set('unserialize_callback_func', $handler);
        }
    }

    protected function doHave(string $id): bool
    {
        stash_get($id, $hit);

        return $hit;
    }

    protected function doClear(string $namespace): bool
    {
        return false; // versioning did the work; there is no prefix delete
    }

    protected function doDelete(array $ids): bool
    {
        foreach ($ids as $id) {
            stash_delete($id);
        }

        return true;
    }

    protected function doSave(array $values, int $lifetime): array|bool
    {
        $failed = [];
        foreach ($values as $id => $v) {
            if (!stash_put($id, $v, $lifetime, true)) {
                $failed[] = $id;
            }
        }

        return $failed;
    }
}
