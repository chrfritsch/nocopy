# Configuration (php.ini)

All settings are `PHP_INI_SYSTEM`: set them in `php.ini` or a `conf.d` file (or `-d` on the command line), not with `ini_set()`.
Set them identically for php-fpm and the CLI.

## Cache

| Setting | Default | Meaning |
|---|---|---|
| `nocopy.shm_name` | (none, **required**) | Name of the POSIX shared-memory object that holds the cache, e.g. `/nocopy`: one leading slash, at most 40 characters. Every process with the same name (and the same `nocopy.so`, run by the same user) shares one cache. Not set, or not attachable: no cache, startup warning |
| `nocopy.shm_size` | `32M` | Arena size, reserved in `/dev/shm` up front. Up to 32G. Processes that attach later use the creator's size. Also sets the shard count ([sizing](../how-to/size-and-monitor.md)) |
| `nocopy.entries_hint` | `4096` | Sizes the fixed slot table (~2 slots per hinted entry, at least one per 256 B of arena). Raise it for many entries under ~300 B |
| `nocopy.local_size` | `8M` | Long-running processes only (CLI, FrankenPHP workers: [copy mode](../explanation/how-it-works.md#zero-copy-and-copy-mode)): per-process budget for keeping private copies of arrays and strings (≥ 512 B) fetched twice, reused while the entry is unchanged. Counts against `memory_limit`. Long-running workers (FrankenPHP worker mode) keep these across requests: size it to their working set, then repeated fetches cost about what zero-copy does. `0` = off |
| `nocopy.serializer` | `php` | `php` or `igbinary`: format for values stored serialized (objects other than plain ones, shared references). Arrays and scalars are never serialized. `igbinary` without the igbinary extension: startup warning, `php` |

The fetch mode is not a setting: zero-copy under php-fpm, CGI, Apache, LiteSpeed, `php -S` and FrankenPHP classic mode, copy mode under the CLI and in FrankenPHP worker scripts (from their first `frankenphp_handle_request()`).

## Valkey

Only in builds with the Valkey tier (default; absent with `--disable-nocopy-valkey`). See [the how-to](../how-to/valkey.md).

| Setting | Default | Meaning |
|---|---|---|
| `nocopy.valkey` | (off) | `tcp://[user:pass@]host:port`, `host:port`, `unix:///path`, or TLS: `tls://…` / `rediss://…`. `phpinfo()` hides the credentials |
| `nocopy.valkey_prefix` | `nocopy:` | Key prefix in Valkey; servers with the same prefix share one cache |
| `nocopy.valkey_timeout` | `0.5` | Seconds to wait for a connect or reply before treating Valkey as down |
| `nocopy.valkey_tls_ca_file` | (system store) | PEM file of the CAs that may sign the server certificate |
| `nocopy.valkey_tls_cert_file` | (none) | Client certificate (chain), for servers with `tls-auth-clients` |
| `nocopy.valkey_tls_key_file` | (none) | Client key |
| `nocopy.valkey_tls_verify` | `1` | Verify the server certificate and its name (or IP); `0` only for tests |

## APCu compatibility

| Setting | Default | Meaning |
|---|---|---|
| `apc.enabled` | `1` | Registered only when APCu is not loaded. Read only by the polyfill's `apcu_enabled()`, for callers that check it (Symfony's `ApcuAdapter`, Composer) |
| `apc.enable_cli` | `0` | Same, for the CLI |

`nocopy_*` ignore both. No other `apc.*` setting has an effect.

## Build options

| `./configure` option | Effect |
|---|---|
| `--enable-nocopy` | Build the extension (default with `phpize`) |
| `--disable-nocopy-valkey` | Leave out the Valkey tier and OpenSSL; no `nocopy.valkey*` settings |
| `--disable-nocopy-lto` | Build without link-time optimization (on for shared builds when the compiler links with `-flto`; fetches 1-4% slower without) |

TLS for Valkey is built when OpenSSL 1.1.1+ (headers and `pkg-config`) is found.
