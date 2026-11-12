# Share a cache across servers with Valkey

Goal: several web servers (and their CLI processes) share one cache.

Valkey holds the data all servers share. Each server keeps local copies in its own shared memory, so most reads never
leave the machine and stay zero-copy. When a value changes, Valkey tells every server to drop its copy (client-side
tracking). nocopy talks to Valkey itself: you do not need phpredis or Predis.

Works with Valkey, or Redis 6.2+ (RESP3), managed services included. Needs a build with Valkey support, which is the
default (not `--disable-nocopy-valkey`).

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

With a password or ACL user: `nocopy.valkey=tcp://user:secret@10.0.0.5:6379`. With a password only:
`tcp://:secret@…` (the `default` user). `redis://` URLs work as they are. `phpinfo()` hides the credentials. To keep the
password out of `php.ini`, use an environment variable: `nocopy.valkey=${NOCOPY_VALKEY}`.

The Valkey user needs access to the keys under the prefix and to `CLIENT TRACKING`. Channel access (`&*`) is optional:
it only keeps idle connections from being closed by Valkey's `timeout`.

All servers must use the same `nocopy.serializer`.

## 2. Restart and verify

Restart php-fpm on each server, then:

```sh
php -r 'var_dump(nocopy_shared_with());'     # "valkey"
valkey-cli client list | grep -c name=nocopy-listener   # one per server
```

Each server runs one listener process (`nocopy-valkey` in `ps`). It receives Valkey's change notifications for the whole
server and drops outdated local copies. Now write on server A and read on server B:

```sh
php -r 'nocopy_put("probe", gethostname());'      # on A
php -r 'var_dump(nocopy_get("probe"));'           # on B: A's hostname
```

## 3. Give Valkey a memory limit

Set `maxmemory` and an eviction policy in `valkey.conf`. A key Valkey evicts is a miss on every server. For the Drupal
module, use `maxmemory-policy volatile-lfu` ([why](../../drupal/README.md#several-web-servers)).

## Use TLS

```ini
nocopy.valkey=tls://user:secret@valkey.example.com:6380    ; or rediss://
nocopy.valkey_tls_ca_file=/etc/ssl/valkey-ca.pem             ; omit to use the system store
; mutual TLS, if the server sets tls-auth-clients:
nocopy.valkey_tls_cert_file=/etc/ssl/nocopy.crt
nocopy.valkey_tls_key_file=/etc/ssl/nocopy.key
```

nocopy uses TLS 1.2 or newer and checks the certificate against the host name (or IP), with SNI. php-fpm's master reads
the files once at startup, so the key file can be readable by root only. Set `nocopy.valkey_tls_verify=0` only in tests.
A build without OpenSSL says so on the first connection.

## When Valkey is down

nocopy stops serving local copies, because they could be outdated. Reads miss and writes return `false`, with a warning
each time; each worker tries to reconnect every 0.5-1.5 s. When the listener reconnects, it clears the local cache, since
it cannot know which changes it missed.

Your code sees cache misses, not exceptions: make sure it can run on misses alone.

## Good to know

- **Reads:** a local hit is zero-copy and costs no round trip. `nocopy_get_multiple()` fetches all local misses in one
  `MGET`; `nocopy_put_multiple()` writes in one pipeline.
- **Writes** cause an invalidation on every server, the writer's included (it fetches its own value back once). That is
  fine for cache traffic, but not for a counter you increment on every request.
- **Per server, not per cluster:** `nocopy_stats()`, and `nocopy_remember()`'s "only one worker computes" (one computation
  per server).
- **Security:** trust Valkey as much as php-fpm. Values read from it are unserialized with every class allowed (as in
  APCu or Symfony's Redis adapter). Use AUTH/ACLs and TLS, and do not share the key prefix with untrusted clients.
- **Clocks:** TTLs go to Valkey as relative times, so Valkey's clock does not matter. Local copies expire at the time the
  writer computed, so run NTP: servers whose clocks differ by more than a TTL keep copies for too short or too long.
- **Not supported yet:** Sentinel, Cluster, TLS over unix sockets.

Background: [Sharing: by name and through Valkey](../explanation/sharing.md).
