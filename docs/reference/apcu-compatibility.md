# APCu compatibility

The polyfill ([`apcu/apcu.php`](../../apcu/apcu.php), `chrfritsch/nocopy-apcu`) defines `apcu_*` on top of `nocopy_*` when nocopy
is loaded and APCu is not. How to set it up: [Migrate from APCu](../how-to/migrate-from-apcu.md).

## Functions

| APCu | Polyfill |
|---|---|
| `apcu_store`, `apcu_add` (key or array) | yes; plain objects stored as property slots, others serialized, as in APCu |
| `apcu_fetch` (key or array), `apcu_exists`, `apcu_delete` (key or array) | yes |
| `apcu_inc`, `apcu_dec`, `apcu_cas` | yes |
| `apcu_clear_cache` | yes; clears the shared cache, php-fpm's included |
| `apcu_entry` | yes, with `nocopy_remember()`'s single flight across workers |
| `apcu_enabled` | yes: shared cache attached, and `apc.enabled` (+ `apc.enable_cli` on the CLI) |
| `apcu_cache_info`, `apcu_sma_info` | totals only, no per-entry or per-segment lists |
| `apcu_key_info` | no |
| `APCUIterator` | no |
| `APC_*` constants | no |

## Settings

Only `apc.enabled` and `apc.enable_cli` exist (registered by the extension when APCu is not loaded). Every other `apc.*`
setting is ignored: size with `nocopy.shm_size` / `nocopy.entries_hint`, serialize with `nocopy.serializer`.
No `apc.slam_defense`, `apc.preload_path`, `apc.use_request_time`.

## APCu's test suite

`./dev apcu` runs APCu's own `.phpt` tests against the polyfill. 23 fail, all expected:

| Tests | Why |
|---|---|
| `iterator_*` (11), `apc54_018`, `ghbug185` | `APCUIterator` |
| `apc_026`, `not_enough_shm` | `apc.*` sizing settings |
| `ghbug335` | `apc.slam_defense` |
| `apc54_014` | `apc.preload_path` |
| `apc_disabled` | warnings of a disabled APCu |
| `apcu_sma_info` | per-segment `apcu_sma_info()` |
| `apc_023`, `apc_099`, `apc_entry_002` | messages and stack traces name `nocopy_*` or the polyfill |
| `apc_store_reference_php8` | outermost reference handling |

## Behaviour

- php-fpm and the CLI share one cache (APCu: the CLI has its own).
- A full cache evicts old, unread entries; APCu wipes everything.
- Calls go through one PHP function each, and values are serialized where APCu serializes, so the polyfill is slower than
  `nocopy_*` and gains nothing where the caller hands over strings (Symfony's `cache.adapter.apcu`).
