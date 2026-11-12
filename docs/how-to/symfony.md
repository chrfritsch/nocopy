# Use nocopy in Symfony

Symfony's built-in `cache.adapter.apcu` serializes every value before it reaches APCu, so it can never read without a copy.
The bundle in [`symfony/`](../../symfony) (`chrfritsch/nocopy-bundle`, for Symfony 6.4, 7.x and 8.x) adds a
`cache.adapter.nocopy` that stores values as they are.

**Before you start:** the extension is [installed](install.md) for php-fpm and the CLI, with the same settings in both:

```ini
nocopy.shm_name=/nocopy
;nocopy.serializer=igbinary   ; if you have the igbinary extension: smaller cached objects
```

The other defaults fit most apps.

## 1. Install the bundle

The bundle is not on Packagist yet. Require it from a checkout of the nocopy repository:

```sh
composer config repositories.nocopy path /path/to/nocopy/symfony
composer require chrfritsch/nocopy-bundle:@dev
```

Register it in `config/bundles.php`:

```php
Chrfritsch\NocopyBundle\NocopyBundle::class => ['all' => true],
```

## 2. Use the adapter

For the app cache:

```yaml
# config/packages/cache.yaml
framework:
    cache:
        app: cache.adapter.nocopy
```

Or for one pool:

```yaml
framework:
    cache:
        pools:
            product.cache:
                adapter: cache.adapter.nocopy
```

Then use the cache as usual:

```php
use Symfony\Contracts\Cache\CacheInterface;
use Symfony\Contracts\Cache\ItemInterface;

public function list(CacheInterface $productCache): array
{
    return $productCache->get('products.page3', function (ItemInterface $item) {
        $item->expiresAfter(300);
        return $this->repository->findPage(3);
    });
}
```

## 3. Check it

```sh
php bin/console cache:pool:clear cache.app
```

Then load a page that uses the cache twice, and check that `nocopy_stats()['hits']` goes up.

## How the adapter behaves

- **Stampede protection:** `$pool->get($key, $callback)` runs on [`nocopy_remember()`](../reference/functions.md#nocopy_remember).
  One worker computes a missing value while the others wait or get the just-expired one, and popular values are refreshed
  shortly before they expire. `expiresAfter()`, `$save = false` and `$beta` work as in Symfony. `setCallbackWrapper()` is
  not used, and `$metadata` comes back empty.
- **Batches:** `getItems()` and deferred saves are one call each; with Valkey, one round trip.
- **Clearing:** `clear()` bumps a namespace version instead of deleting keys. Old entries leave through eviction, or, with
  Valkey, through a one-year TTL given to items without a lifetime.
- **System pools:** Symfony's system pools (validator, serializer, property info) get nocopy in front of their PHP files,
  as Symfony does with APCu. Setting `framework.cache.system` in your config overrides this.
- **Deploys:** each new container build (deploy, `cache:clear`) starts the pools empty, as with APCu.
- **Safety:** the adapter refuses to start in a process without a shared cache (`nocopy_shared_with()` returns `none`).
  Set `nocopy.shm_name` (or `nocopy.valkey`) for php-fpm and the CLI, and run the console as the php-fpm user.
- **APCu:** the adapter only calls `nocopy_*`, so it works next to a loaded APCu.

## FrankenPHP worker mode

In worker mode the kernel lives across requests, so nocopy switches to copy mode by itself. Values a worker reads often
are kept per worker, up to `nocopy.local_size` (`8M`, counted against `memory_limit`). Raise it if the app's hot cache
entries add up to more; `nocopy_stats()['used']` after warm-up is an upper bound.
