# Configuration (php.ini)

All settings are `PHP_INI_SYSTEM`: set them in `php.ini` or a `conf.d` file (or with `-d` on the command line), not with
`ini_set()`. Give php-fpm and the CLI the **same** values, or they will not share the cache.

A typical setup:

```ini
extension=nocopy
nocopy.shm_name=/nocopy
nocopy.shm_size=128M
```

## Cache

| Setting | Default | In short |
|---|---|---|
| [`nocopy.shm_name`](#nocopyshm_name) | none, **required** | Name of the cache; processes with the same name share it |
| [`nocopy.shm_size`](#nocopyshm_size) | `32M` | Memory for the cache, reserved up front |
| [`nocopy.entries_hint`](#nocopyentries_hint) | `4096` | Expected number of entries; raise it for many small ones |
| [`nocopy.zero_copy_seconds`](#nocopyzero_copy_seconds) | `3` | After how long a web request switches to copying |
| [`nocopy.local_size`](#nocopylocal_size) | `8M` | Per-process memory for reused copies (CLI and workers) |
| [`nocopy.serializer`](#nocopyserializer) | `php` | `php` or `igbinary`, for values that must be serialized |

### `nocopy.shm_name`

The name of the POSIX shared-memory object that holds the cache, for example `/nocopy`: one leading slash, at most 40
characters. Every process with the same name, the same `nocopy.so` build and the same user shares one cache
([why these rules](../explanation/sharing.md#one-user-one-build-one-machine)).

If it is not set, or the cache cannot be opened, the process has **no cache**: it logs a startup warning, every read misses
and every write returns `false`. There is no default on purpose, so two sites never share keys by accident.

- Up to 1024 processes and threads can share one name. Beyond that, a process has no cache (a warning, and
  `nocopy_stats()['off']` says why).
- When php-fpm's master or Apache's parent runs as root, each worker opens the cache on its first request, as the worker's
  user. A CLI command run as that same user shares it.
- Under php-fpm you can set a different name per pool, with `php_admin_value[nocopy.shm_name]`.

Problems are written to the error log.

### `nocopy.shm_size`

How much memory the cache gets, up to `32G`. The whole size is reserved in `/dev/shm` when the cache is created. Processes
that open the cache later use the creator's size, whatever they have configured. It also decides how many parts (shards)
the cache is split into, which limits the size of a single entry ([sizing](../how-to/size-and-monitor.md)).

Too small to hold the slot table (see `nocopy.entries_hint`): startup warning, no cache. Negative: startup warning, the
default is used.

### `nocopy.entries_hint`

The number of entries you expect. The slot table that finds keys has a fixed size, set at startup: about 2 slots per
hinted entry, and at least one slot per 256 bytes of cache memory. Raise it if you store many entries under ~300 bytes
([how to tell](../how-to/size-and-monitor.md#many-small-entries-raise-nocopyentries_hint)).

### `nocopy.zero_copy_seconds`

Web requests (php-fpm, mod_php, `php -S`, classic FrankenPHP) get values without a copy. Memory they read from cannot be
reused until the request ends, so a request older than this many seconds copies the values it fetches from then on. What
it already holds stays valid until it ends.

`0` removes the age limit; only the cache-wide limit applies then (requests may hold at most a quarter of the cache's
segments). Capped at 10^9.

### `nocopy.local_size`

For long-running processes only: CLI scripts and FrankenPHP workers, which run in
[copy mode](../explanation/how-it-works.md#zero-copy-and-copy-mode). When such a process fetches the same array or string
(512 bytes or more) twice, it keeps its private copy and hands it out again while the entry is unchanged. This setting is
the budget for those copies, per process. It counts heap bytes against `memory_limit`.

FrankenPHP workers keep these copies across requests: size it to the data they read often, and repeated reads cost about
as much as zero-copy reads. `0` turns it off. Negative: startup warning, the default is used.

### `nocopy.serializer`

`php` or `igbinary`. Used for values that must be serialized: objects that cannot be stored as properties (see
[What can be stored](functions.md#what-can-be-stored)). Arrays and scalars are never serialized in the local cache.
The Valkey tier serializes every value except integers, so there `igbinary` makes arrays 2-3x smaller.

`igbinary` without the igbinary extension loaded: startup warning, `php` is used.

### Zero-copy or copy mode: not a setting

nocopy picks the mode from how PHP runs:

| Zero-copy | Copy mode |
|---|---|
| php-fpm, CGI, Apache, LiteSpeed, `php -S`, FrankenPHP classic mode | CLI, FrankenPHP worker scripts (from their first `frankenphp_handle_request()`) |

## Valkey

Only in builds with the Valkey tier (the default; absent with `--disable-nocopy-valkey`). How to set it up:
[Share a cache across servers with Valkey](../how-to/valkey.md).

| Setting | Default | Meaning |
|---|---|---|
| `nocopy.valkey` | off | Where Valkey is (formats below). `phpinfo()` hides the credentials |
| `nocopy.valkey_prefix` | `nocopy:` | Key prefix in Valkey. Servers with the same prefix share one cache |
| `nocopy.valkey_timeout` | `0.5` | Seconds to wait for a connection or a reply before treating Valkey as down |
| `nocopy.valkey_tls_ca_file` | system store | PEM file with the CAs that may sign the server certificate |
| `nocopy.valkey_tls_cert_file` | none | Client certificate (chain), for servers with `tls-auth-clients` |
| `nocopy.valkey_tls_key_file` | none | Client key |
| `nocopy.valkey_tls_verify` | `1` | Verify the server certificate and its host name (or IP). `0` only for tests |

`nocopy.valkey` accepts:

```ini
nocopy.valkey=10.0.0.5:6379
nocopy.valkey=tcp://10.0.0.5:6379
nocopy.valkey=tcp://user:secret@10.0.0.5:6379   ; ACL user
nocopy.valkey=tcp://:secret@10.0.0.5:6379       ; password only: the "default" user
nocopy.valkey=redis://user:secret@10.0.0.5:6379
nocopy.valkey=unix:///run/valkey.sock
nocopy.valkey=tls://user:secret@valkey.example.com:6380   ; or rediss://
```

## APCu compatibility

Registered only when APCu is not loaded, for the [polyfill](../how-to/migrate-from-apcu.md):

| Setting | Default | Meaning |
|---|---|---|
| `apc.enabled` | `1` | Read only by the polyfill's `apcu_enabled()`, for callers that check it (Symfony's `ApcuAdapter`, Composer) |
| `apc.enable_cli` | `0` | Same, for the CLI |

The `nocopy_*` functions ignore both. No other `apc.*` setting has any effect.

## Build options

| `./configure` option | Effect |
|---|---|
| `--enable-nocopy` | Build the extension (the default with `phpize`) |
| `--disable-nocopy-valkey` | Leave out the Valkey tier and OpenSSL; no `nocopy.valkey*` settings |
| `--disable-nocopy-lto` | Build without link-time optimization (on by default for shared builds when the compiler supports `-flto`; reads are 1-4% slower without) |

TLS for Valkey is built in when OpenSSL 1.1.1+ (headers and `pkg-config`) is found. With PIE, pass the same options:
`pie install chrfritsch/nocopy --disable-nocopy-valkey`.
