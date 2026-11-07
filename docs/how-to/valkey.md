# Share a cache across servers with Valkey

Goal: several web servers (and their CLI processes) share one cache. Valkey becomes the cache of record; each server keeps
zero-copy local copies in front of it, kept in sync by Valkey's client-side tracking. nocopy speaks to Valkey itself, no
phpredis needed.

Works with Valkey, or Redis 6.2+, with RESP3, managed services included. Requires a build with the Valkey tier (the default;
not `--disable-nocopy-valkey`).

## 1. Point every server at Valkey

On every server, for php-fpm and the CLI:

```ini
extension=nocopy.so
nocopy.shm_name=/nocopy                 ; still required: holds this server's local copies
nocopy.shm_size=128M
nocopy.valkey=tcp://10.0.0.5:6379       ; or host:port, unix:///run/valkey.sock
;nocopy.valkey_prefix=nocopy:           ; servers with the same prefix share one cache
;nocopy.valkey_timeout=0.5              ; seconds before Valkey counts as down
```

With a password or ACL user: `nocopy.valkey=tcp://user:secret@10.0.0.5:6379`. `phpinfo()` hides the credentials.

All servers must use the same `nocopy.serializer`.

## 2. Restart and verify

Restart php-fpm on each server, then:

```sh
php -r 'var_dump(nocopy_shared_with());'     # "valkey"
valkey-cli client list | grep -c name=nocopy-listener   # one per server
```

Each server runs one listener process (`nocopy-valkey` in `ps`) that receives invalidations for the whole host.
Write on server A, read on server B:

```sh
php -r 'nocopy_put("probe", gethostname());'      # on A
php -r 'var_dump(nocopy_get("probe"));'           # on B: A's hostname
```

## 3. Give Valkey a memory limit

Set `maxmemory` and an eviction policy in `valkey.conf`; an evicted key is a miss on every server. For the Drupal module use
`maxmemory-policy volatile-lfu` ([why](../../drupal/README.md)).

## Use TLS

```ini
nocopy.valkey=tls://user:secret@valkey.example.com:6380    ; or rediss://
nocopy.valkey_tls_ca_file=/etc/ssl/valkey-ca.pem             ; omit to use the system store
; mutual TLS, if the server sets tls-auth-clients:
nocopy.valkey_tls_cert_file=/etc/ssl/nocopy.crt
nocopy.valkey_tls_key_file=/etc/ssl/nocopy.key
```

TLS 1.2+, the certificate is checked against the host name (or IP), with SNI. The files are read once at startup by
php-fpm's master, so the key can be readable by root only. `nocopy.valkey_tls_verify=0` only for tests.
A build without OpenSSL says so on the first connect.

## When Valkey is down

nocopy serves nothing local (it could be stale): fetches miss, writes return `false`, with one warning per attempt; each
worker retries every 0.5-1.5 s. When the listener reconnects it clears the local cache, since it cannot know what it missed.
Plan for misses, not errors.

## Good to know

- Local hits are zero-copy and cost no round trip. `nocopy_get_multiple()` fetches all local misses in one `MGET`,
  `nocopy_put_multiple()` writes in one pipeline.
- Each write is pushed back to every server, the writer's included (its copy is refetched once): fine for cache traffic,
  not for counters incremented on every request.
- Local only: `nocopy_stats()`, and the single flight of `nocopy_remember()` (one computation per server, not per cluster).
- Trust Valkey as much as php-fpm: values read from it are unserialized with every class allowed (as in APCu or
  Symfony's Redis adapter). Use AUTH/ACLs and TLS, and do not share the key prefix with untrusted clients.
- Not supported yet: Sentinel, Cluster, TLS over unix sockets.

Background: [Sharing: by name and through Valkey](../explanation/sharing.md).
