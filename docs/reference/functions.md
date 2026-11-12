# Functions

Every function the extension defines, with its signature from [`nocopy.stub.php`](../../nocopy.stub.php).

## Before you start

- **No cache, no errors.** When a process has no shared cache (`nocopy.shm_name` is not set, or the cache could not be
  opened), every function still works: reads miss, writes return `false`, and `nocopy_remember()` computes the value
  without storing it. Only invalid arguments throw, such as a negative TTL. `nocopy_stats()['off']` tells you why the
  cache is off.
- **TTL** is in seconds. `0` means "no expiry". Values above 2^32 seconds (about 136 years) are capped there. As in APCu,
  an entry lives until the end of the second it expires in, so `ttl: 1` keeps it for 1 to 2 seconds.
- **Keys** are hashed with PHP's own unseeded string hash, as in APCu. That means someone can craft keys that collide, and
  colliding keys slow down every write in their part of the cache. If a key contains user input, hash it with a secret
  first ([why](../explanation/design-decisions.md#keys-use-phps-own-hash-without-a-secret-seed)):

  ```php
  $key = 'search.' . hash_hmac('xxh128', $_GET['q'], $secret);
  ```

| Function | What it does |
|---|---|
| [`nocopy_get`](#nocopy_get) | Read one value |
| [`nocopy_get_multiple`](#nocopy_get_multiple) | Read several values |
| [`nocopy_put`](#nocopy_put) | Store a value |
| [`nocopy_add`](#nocopy_add) | Store a value only if the key is missing |
| [`nocopy_put_multiple`](#nocopy_put_multiple) | Store several values |
| [`nocopy_delete`](#nocopy_delete) | Delete one key |
| [`nocopy_delete_multiple`](#nocopy_delete_multiple) | Delete several keys |
| [`nocopy_clear`](#nocopy_clear) | Delete everything |
| [`nocopy_inc`](#nocopy_inc) | Add to an integer counter |
| [`nocopy_cas`](#nocopy_cas) | Compare-and-swap an integer |
| [`nocopy_remember`](#nocopy_remember) | Return a cached value or compute it once |
| [`nocopy_shared_with`](#nocopy_shared_with) | Who shares this process's cache |
| [`nocopy_stats`](#nocopy_stats) | Counters and sizes |
| [`nocopy_keys`](#nocopy_keys) | List keys |

## Read

### `nocopy_get`

```php
nocopy_get(string $key, mixed &$hit = null): mixed
```

Returns the value, or `null` on a miss. Pass `$hit` to tell a miss apart from a stored `null` or `false`: it is set to
`true` on a hit.

```php
$config = nocopy_get('config', $hit);
if (!$hit) {
    $config = loadConfig();
    nocopy_put('config', $config, ttl: 300);
}
```

Under web servers the value is returned without copying it; in CLI scripts and worker processes you get a private copy
([zero-copy and copy mode](../explanation/how-it-works.md#zero-copy-and-copy-mode)). Either way, your code can modify the
result like any other array.

### `nocopy_get_multiple`

```php
nocopy_get_multiple(array $keys): array
```

Returns the keys that were found, as `key => value`. Missing keys are left out.

```php
$users = nocopy_get_multiple(['user.1', 'user.2', 'user.3']);
$missing = array_diff(['user.1', 'user.2', 'user.3'], array_keys($users));
```

With `nocopy.resp`, all keys missing locally are fetched from RESP in one round trip (`MGET`).

## Write

### `nocopy_put`

```php
nocopy_put(string $key, mixed $value, int $ttl = 0): bool
```

Stores `$value` under `$key`, replacing what was there. Returns `false` if the value does not fit (it is larger than one
part of the cache, see [sizing](../how-to/size-and-monitor.md#pick-nocopyshm_size)) or there is no shared cache.
A negative `$ttl` throws a `ValueError`; a resource warns and returns `false`.

```php
nocopy_put('routes', $routes);              // no expiry
nocopy_put('weather', $forecast, ttl: 600); // 10 minutes
```

What happens to the value depends on its type; see [What can be stored](#what-can-be-stored) below.

### `nocopy_add`

```php
nocopy_add(string $key, mixed $value, int $ttl = 0): bool
```

Like `nocopy_put()`, but only stores if the key is missing (or expired). Returns `false` if the key exists.

```php
if (nocopy_add('job.42.lock', getmypid(), ttl: 30)) {
    // this process won
}
```

### `nocopy_put_multiple`

```php
nocopy_put_multiple(array $values, int $ttl = 0): bool
```

Stores every `key => value` pair in `$values`, all with the same `$ttl`. Returns `false` if some were not stored.
With `nocopy.resp`, all writes go out in one pipeline.

### `nocopy_delete`

```php
nocopy_delete(string $key): bool
```

Deletes the key. Returns `true` if it existed.

### `nocopy_delete_multiple`

```php
nocopy_delete_multiple(array $keys): int
```

Deletes `$keys` and returns how many of them existed. Entries that are not strings are skipped.
With `nocopy.resp`, one `UNLINK` command.

### `nocopy_clear`

```php
nocopy_clear(): bool
```

Deletes every entry. With `nocopy.resp`, it also deletes every key under `nocopy.resp_prefix` in RESP.

Returns `false` if not everything was cleared: the cache is off in this process (`nocopy_stats()['off']` says why), or
RESP could not be reached. Use this in deploy scripts so a clear that reached nothing fails loudly:

```sh
sudo -u www-data php -r 'exit(nocopy_clear() ? 0 : 1);'
```

## Counters

### `nocopy_inc`

```php
nocopy_inc(string $key, int $step = 1, int $ttl = 0): int|false
```

Adds `$step` to an integer, atomically, and returns the new value. Use a negative `$step` to subtract. A missing key
starts at `$step`, and `$ttl` applies only then, when the key is created.

Returns `false` if the stored value is not an integer, or there is no room. A negative `$ttl` throws a `ValueError`.

```php
$views = nocopy_inc('views.' . $id);
nocopy_inc('stock.' . $sku, -1);
```

### `nocopy_cas`

```php
nocopy_cas(string $key, int $old, int $new): bool
```

Replaces the integer `$old` with `$new`, atomically. Returns `false` if the current value is not `$old`.

```php
// Bump the version only if no other process did in the meantime.
$version = nocopy_get('schema.version');
if (nocopy_cas('schema.version', $version, $version + 1)) {
    migrate($version + 1);
}
```

## Compute once

### `nocopy_remember`

```php
nocopy_remember(string $key, callable $callback, int $ttl = 0, float $beta = 1.0): mixed
```

Returns the cached value. On a miss, it calls `$callback($key)`, stores the result for `$ttl` seconds and returns it.
This is the replacement for the "get, check, compute, put" pattern, and it protects you from cache stampedes.

```php
$report = nocopy_remember('report.daily', fn () => buildReport(), ttl: 3600);
```

**Change the TTL from the callback.** A user callback (closure, function or method) also receives `int &$ttl`, set to
`$ttl`. Change it to store the value for a different time; set it below `0` to return the value without storing it.
Internal functions such as `'strtoupper'` receive only the key.

```php
$page = nocopy_remember('page.' . $id, function (string $key, int &$ttl) use ($id) {
    $page = fetchPage($id);
    if ($page === null) {
        $ttl = -1;          // don't cache "not found"
    } elseif ($page->isDraft()) {
        $ttl = 10;
    }
    return $page;
}, ttl: 600);
```

**Only one worker computes.** On a miss, the first worker takes a lease and runs the callback; the others wait for its
result instead of all computing the same thing. If the computing worker dies, the others notice; if it is stuck, they
stop waiting after 30 s. A recursive call for the same key does not wait for itself. With `nocopy.resp`, this works
per server, not across the cluster.

**Stale while revalidate.** While one worker recomputes an expired value, the waiting workers get the expired value
instead, as long as it expired less than one TTL ago (so it is at most 2 × TTL old). A value that was deleted or
invalidated is never served stale. Not with `nocopy.resp`, and not with `$beta = 0`.

**Early refresh.** Shortly before the TTL ends, one worker recomputes the value while the others keep getting the current
one, so a popular key usually never expires at all. The chance of refreshing rises as expiry nears, and is scaled by how
long the callback takes × `$beta` ([XFetch](https://doi.org/10.14778/2757807.2757813)). `$beta = 0` turns off early
refresh and stale serving.

The callback can return anything `nocopy_put()` can store, objects included.

## Introspection

### `nocopy_shared_with`

```php
nocopy_shared_with(): string
```

Who sees this process's writes:

| Value | Meaning |
|---|---|
| `host` | Every process on this machine that uses the same `nocopy.shm_name` |
| `resp` | Every process, on any server, that uses the same `nocopy.resp` and `nocopy.resp_prefix` |
| `none` | No one: there is no shared cache, nothing is stored or returned |

Cheap (about 15 ns), so it is fine to call on every request, for example to refuse to run with a cache that is off:

```php
if (nocopy_shared_with() === 'none') {
    throw new RuntimeException('nocopy cache is off: ' . nocopy_stats()['off']);
}
```

### `nocopy_stats`

```php
nocopy_stats(): array
```

Counters and sizes of the whole cache, for all processes: see [`nocopy_stats()`](stats.md).

### `nocopy_keys`

```php
nocopy_keys(string $prefix = ""): array
```

The live keys that start with `$prefix`, oldest first (to within 1-4 ms, so not exact), as

```php
[
    'user.42' => ['size' => 1840, 'created' => 1762934400, 'expires' => 1762934700], // expires 0: never
]
```

`size` is the entry's size in bytes; `created` and `expires` are Unix timestamps, accurate to about a second.

It does not block writers, so a key that is being deleted at the same moment may be missing from the list. It looks at
every slot in the cache, so use it in tools and admin pages, not on every request. With `nocopy.resp` it lists what
this server has cached locally, not every key in RESP.

## What can be stored

| Value | How it is stored | On fetch |
|---|---|---|
| Arrays, strings, integers, floats, booleans, `null` | As they are | Zero-copy under web servers |
| Most objects (see below) | As their properties, without serializing | Rebuilt; the strings and arrays inside stay zero-copy |
| `Serializable` objects, lazy objects, internal classes without `__serialize()` | Serialized with `nocopy.serializer`, as in APCu | Unserialized every time |
| Values nested more than 64 levels deep | Serialized, as above | Unserialized every time |
| Resources | Not stored: a warning, and `nocopy_put()` returns `false` | |

**Objects without serializing.** Objects are rebuilt on fetch with the same result, and the same `__wakeup()` /
`__unserialize()` calls in the same order, as `unserialize()` would give you:

- User classes and `stdClass`: their properties, including dynamic ones. `__wakeup()` runs on fetch if it is declared.
- Classes with `__sleep()`: the properties it names; the others get their class default, as with `unserialize()`.
  If `__sleep()` names something `serialize()` would warn about, the value is stored serialized, with that warning.
- Classes with `__serialize()` / `__unserialize()`, internal ones included (`ArrayObject`, `SplObjectStorage`, ...): the
  array `__serialize()` returns.
- Enums: their case.
- `DateTime` and `DateTimeImmutable`: timestamp and time zone. If that zone is no longer in the time zone database, the
  fetch is a miss.

Object identity, cycles, private, readonly and uninitialized properties are kept. A typed property you `unset()` comes back
uninitialized, as from `unserialize()`, so `__get()` is not called for it.

**References.** A PHP reference that appears more than once in the value is still one reference after a fetch: writing
through one place shows in the others. Its value stays zero-copy, but the arrays that hold it are copied.

**When a fetch becomes a miss.** If the class was changed since the value was stored (a different property layout),
cannot be loaded, or no longer has the enum case, the fetch is a miss. If `__wakeup()` or `__unserialize()` throws, the
fetch is a miss and the exception is thrown.

## APCu names

The extension does not define `apcu_*` functions; the [polyfill](../how-to/migrate-from-apcu.md) does, in PHP. The
extension only registers the `apc.enabled` and `apc.enable_cli` settings (when APCu is not loaded), so code that checks
them keeps working.
