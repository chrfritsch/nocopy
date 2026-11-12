# Migrate from APCu

There are two ways. Pick by how much of the code you control:

- **Route A:** replace `apcu_*` calls with `nocopy_*`. Full speed. For your own code.
- **Route B:** keep the `apcu_*` calls and load a polyfill. No code changes, smaller gain. For libraries and legacy code.

You can combine them: the polyfill for code you can't change, `nocopy_*` for the hot paths you own.

## Route A: switch to `nocopy_*` (full speed)

Most calls map one to one:

| APCu | nocopy |
|---|---|
| `apcu_store($k, $v, $ttl)` | `nocopy_put($k, $v, $ttl)` |
| `apcu_fetch($k, $ok)` | `nocopy_get($k, $ok)` |
| `apcu_add` / `apcu_delete` / `apcu_clear_cache` | `nocopy_add` / `nocopy_delete` / `nocopy_clear` |
| `apcu_inc` / `apcu_dec` / `apcu_cas` | `nocopy_inc($k, $step)` (negative to subtract) / `nocopy_cas` |
| `apcu_entry($k, $fn, $ttl)` | `nocopy_remember($k, $fn, $ttl)` (single flight across workers) |
| `apcu_fetch(array $keys)` / `apcu_store(array)` | `nocopy_get_multiple` / `nocopy_put_multiple` |

Before and after:

```php
// APCu
$rows = apcu_fetch('products', $ok);
if (!$ok) {
    $rows = loadProducts();
    apcu_store('products', $rows, 300);
}

// nocopy
$rows = nocopy_remember('products', fn () => loadProducts(), ttl: 300);
```

Arrays and scalars are never serialized: that is where zero-copy comes from. Most objects are not serialized either. They
are stored as their properties and rebuilt on fetch, with the same `__sleep()`, `__wakeup()` and `__unserialize()` calls
`serialize()` and `unserialize()` would make, and the strings and arrays inside them stay zero-copy. Internal value classes
(dates, SPL containers, `ArrayObject`) are nodes too. Objects that implement only `Serializable`, exceptions and
stateful internal objects (iterators, DOM, XML readers) are stored serialized, as in APCu
([details](../reference/functions.md#what-can-be-stored)).

For frameworks: [Symfony bundle](symfony.md), [Drupal module](../../drupal/README.md).

## Route B: the polyfill (no code changes)

For code that calls `apcu_*` and cannot change yet (libraries, legacy code).

1. Load nocopy **instead of** APCu (remove `extension=apcu`), with `nocopy.shm_name` set ([install](install.md)).
2. Require the polyfill. With Composer, until it is on Packagist:

   ```sh
   composer config repositories.nocopy-apcu path /path/to/nocopy/apcu
   composer require chrfritsch/nocopy-apcu:@dev
   ```

   The autoloader loads it. Without Composer: `require '/path/to/nocopy/apcu/apcu.php';`.
3. Keep `apc.enabled=1`, and `apc.enable_cli=1` if the CLI uses the cache: callers like Symfony's `ApcuAdapter` and
   Composer check these before calling `apcu_*`.

The polyfill defines `apcu_*` only when nocopy is loaded and APCu is not. Each call goes through a PHP function and objects
are stored serialized, so the gain is smaller than with `nocopy_*`. Where the caller serializes values itself (Symfony's
`cache.adapter.apcu` hands over strings), there is no gain: use the [Symfony bundle](symfony.md) instead.

What is missing (`apcu_key_info()`, most `apc.*` settings): [APCu compatibility](../reference/apcu-compatibility.md).

## Sizing

APCu's `apc.shm_size` and `apc.entries_hint` have no effect. Use `nocopy.shm_size` and `nocopy.entries_hint` instead
([Size and monitor the cache](size-and-monitor.md)).

## Behaviour that differs

- **A full cache** removes old entries that nobody read and keeps the popular ones. APCu wipes everything.
- **php-fpm and the CLI share one cache.** With APCu each CLI process has its own, so `apcu_clear_cache()` in a CLI script
  did nothing to the web cache. With nocopy it clears the web cache too.
- **A process without the shared cache** (no `nocopy.shm_name`) has no cache at all, rather than a private one.
- **CLI scripts need the right user.** Run them as the php-fpm pool's user, or they cannot open the cache
  ([details](share-between-fpm-and-cli.md)).
