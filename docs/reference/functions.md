# Functions

All functions work without a shared cache too (`nocopy.shm_name` unset or not attached): fetches miss, writes return `false`,
`nocopy_remember()` computes without keeping. Errors in the arguments (an object without `$serialize`, a negative TTL) still throw.

Signatures come from [`nocopy.stub.php`](../../nocopy.stub.php).

`$ttl` is in seconds, `0` = no expiry. Like APCu, an entry lives through the whole second it expires in: `ttl: 1` lives 1-2 s.

## Read

### `nocopy_get(string $key, mixed &$hit = null): mixed`

The value, or `null` on a miss. `$hit` is `true` on a hit, so a stored `null`/`false` is told apart from a miss.
Under web SAPIs returns the value without copying it ([fetch modes](../explanation/how-it-works.md#zero-copy-and-copy-mode)).

### `nocopy_get_multiple(array $keys): array`

The hits as `key => value`; missing keys are left out. With `nocopy.valkey`, all local misses in one round trip (`MGET`).

## Write

### `nocopy_put(string $key, mixed $value, int $ttl = 0, bool $serialize = false): bool`

Stores `$value`. Arrays, strings, integers, floats, booleans and `null` are stored as they are (zero-copy on fetch).
A value containing an object throws a `TypeError`, unless `$serialize: true` stores it serialized (with `nocopy.serializer`).
Arrays with references shared between elements are stored serialized automatically. Resources throw a `TypeError`.
A negative `$ttl` throws a `ValueError`. Returns `false` when the value does not fit (larger than a shard) or there is no shared cache.

### `nocopy_add(string $key, mixed $value, int $ttl = 0, bool $serialize = false): bool`

Like `nocopy_put()`, only if the key is missing (or expired). `false` if it exists.

### `nocopy_put_multiple(array $values, int $ttl = 0, bool $serialize = false): bool`

`key => value`, one `$ttl` for all. `false` if some were not stored. With `nocopy.valkey`, one pipeline.

### `nocopy_delete(string $key): bool`

`true` if the key existed.

### `nocopy_clear(): bool`

Removes every entry. With `nocopy.valkey`, every key under `nocopy.valkey_prefix` in Valkey too.

## Counters

### `nocopy_inc(string $key, int $step = 1, int $ttl = 0): int|false`

Adds `$step` (negative subtracts) to an integer in place, atomically; returns the new value. A missing key starts at `$step`.
`false` if the value is not an integer, or there is no room.

### `nocopy_cas(string $key, int $old, int $new): bool`

Replaces the integer `$old` with `$new` atomically; `false` if the current value is not `$old`.

## Compute once

### `nocopy_remember(string $key, callable $callback, int $ttl = 0, float $beta = 1.0): mixed`

Returns the cached value, or computes it with `$callback($key)`, stores it and returns it.

- **Single flight:** on a miss, the first worker takes a lease and runs the callback; the others wait for its value.
  A dead lease holder is detected, a stuck one is ignored after 30 s, and a recursive call for the same key does not wait.
  With `nocopy.valkey` the single flight is per server.
- **Stale while revalidate:** while another worker recomputes, a waiter gets the expired value instead, if it expired less
  than one TTL ago (so it is at most 2 × TTL old). Deleted or invalidated keys are never served stale. Not with `nocopy.valkey`, nor with `$beta = 0` (so `apcu_entry` never returns an expired value).
- **Early refresh:** before the TTL ends, one worker recomputes early ([XFetch](https://doi.org/10.14778/2757807.2757813))
  with a probability that rises as expiry nears, scaled by the callback's measured run time × `$beta`. The others keep
  getting the current value. `$beta = 0` turns this and stale serving off.
- The callback may return anything storable, objects included (stored serialized).

## Introspection

### `nocopy_shared_with(): string`

Who sees this process's writes:

| Value | Meaning |
|---|---|
| `host` | every process on this machine attached to the same `nocopy.shm_name` |
| `valkey` | every process on the same `nocopy.valkey` and `nocopy.valkey_prefix` |
| `none` | no shared cache: nothing is kept or served |

Cheap (about 15 ns): fine to call on every request.

### `nocopy_stats(): array`

Counters and sizes of the arena: see [`nocopy_stats()`](stats.md).

## APCu names

`apcu_*` are not part of the extension; the [polyfill](../how-to/migrate-from-apcu.md) defines them in PHP.
The extension registers only `apc.enabled` and `apc.enable_cli` (when APCu is not loaded).
