# Use nocopy in Symfony

Symfony's built-in `cache.adapter.apcu` serializes values before they reach the extension, so it cannot be zero-copy. The
bundle in [`symfony/`](../../symfony) (`chrfritsch/nocopy-bundle`, Symfony 6.4 / 7.x / 8.x) adds `cache.adapter.nocopy`,
which stores values as they are.

Prerequisite: the extension is [installed](install.md) for php-fpm and the CLI.

## 1. Install the bundle

Until it is on Packagist, from a checkout:

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

## 3. Verify

```sh
php bin/console cache:pool:clear cache.app
```

then load a page that uses the cache twice and check `nocopy_stats()['hits']` grows.

## Notes

- The adapter calls `nocopy_*` only, so it works next to a loaded APCu.
- `clear()` bumps a namespace version instead of deleting keys; old entries leave through eviction (shm) or a one-year TTL
  given to items without a lifetime (Valkey).
- The adapter refuses to start in a process that does not share the cache (`nocopy_shared_with()` is `none`): set
  `nocopy.shm_name` or `nocopy.valkey` for php-fpm and the CLI, and run the console as the php-fpm user.
- Multi-key reads and writes (`getItems()`, deferred saves) are one call each; with Valkey, one round trip.
- Each new container build (deploy, `cache:clear`) starts the pools empty, as with APCu.
- Under FrankenPHP worker mode, nocopy switches to copy mode automatically (the kernel keeps values across requests).
