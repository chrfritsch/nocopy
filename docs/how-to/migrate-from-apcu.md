# Migrate from APCu

Two routes. Pick by how much code you control.

## Route A: switch to `nocopy_*` (full speed)

Replace calls one to one:

| APCu | nocopy |
|---|---|
| `apcu_store($k, $v, $ttl)` | `nocopy_put($k, $v, $ttl)` |
| `apcu_fetch($k, $ok)` | `nocopy_get($k, $ok)` |
| `apcu_add` / `apcu_delete` / `apcu_clear_cache` | `nocopy_add` / `nocopy_delete` / `nocopy_clear` |
| `apcu_inc` / `apcu_dec` / `apcu_cas` | `nocopy_inc($k, $step)` (negative to subtract) / `nocopy_cas` |
| `apcu_entry($k, $fn, $ttl)` | `nocopy_remember($k, $fn, $ttl)` (single flight across workers) |
| `apcu_fetch(array $keys)` / `apcu_store(array)` | `nocopy_get_multiple` / `nocopy_put_multiple` |

Arrays and scalars are never serialized: that is where zero-copy comes from. Most objects aren't either: a fetch rebuilds
them from their stored properties (or `__serialize()` array, or enum case), whose strings and arrays stay zero-copy, and runs
`__wakeup`/`__unserialize` as `unserialize()` would. Objects with `__sleep` or `Serializable` are stored serialized, as in APCu.

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

The polyfill defines `apcu_*` only when nocopy is loaded and APCu is not. Every call goes through one PHP function and
objects are stored serialized, so the gain is smaller than with `nocopy_*`, and none where the caller serializes first
(Symfony's `cache.adapter.apcu` hands over strings).

What is missing (`APCUIterator`, `apcu_key_info()`, most `apc.*` settings): [APCu compatibility](../reference/apcu-compatibility.md).

## Sizing

APCu's `apc.shm_size` and `apc.entries_hint` do nothing; use `nocopy.shm_size` and `nocopy.entries_hint`
([Size and monitor the cache](size-and-monitor.md)).

## Behaviour that differs

- **A full cache** evicts its oldest segments and keeps entries that are read; APCu wipes everything.
- **php-fpm and the CLI share one cache** (APCu: each CLI process has its own), so `apcu_clear_cache()` in a CLI script
  clears the web cache too.
- **A process without the shared cache** (no `nocopy.shm_name`) has no cache at all, rather than a private one.
