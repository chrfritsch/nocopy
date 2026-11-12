# Share a cache across servers with RESP

Goal: several web servers (and their CLI processes) share one cache.

RESP is the protocol that Redis and Valkey speak, so any of them works as the shared cache. The RESP server holds the data all servers share. Each server keeps local copies in its own shared memory, so most reads never
leave the machine and stay zero-copy. When a value changes, the RESP server tells every web server to drop its copy (client-side
tracking). nocopy talks to the RESP server itself: you do not need phpredis or Predis.

Works with any server that speaks RESP: Redis 6.2+ or Valkey, managed services included. Needs a build with the RESP tier, which is the
default (not `--disable-nocopy-resp`).

## 1. Point every server at the RESP server

On every server, for php-fpm and the CLI:

```ini
extension=nocopy.so
nocopy.shm_name=/nocopy                 ; still required: holds this server's local copies
nocopy.shm_size=128M
nocopy.resp=tcp://10.0.0.5:6379       ; or host:port, unix:///run/resp.sock
;nocopy.resp_prefix=nocopy:           ; servers with the same prefix share one cache
;nocopy.resp_timeout=0.5              ; seconds before RESP counts as down
```

With a password or ACL user: `nocopy.resp=tcp://user:secret@10.0.0.5:6379`. With a password only:
`tcp://:secret@…` (the `default` user). `redis://` URLs work as they are. `phpinfo()` hides the credentials. To keep the
password out of `php.ini`, use an environment variable: `nocopy.resp=${NOCOPY_RESP}`.

The RESP user needs access to the keys under the prefix and to `CLIENT TRACKING`. Channel access (`&*`) is optional:
it only keeps idle connections from being closed by RESP's `timeout`.

All servers must use the same `nocopy.serializer`.

## 2. Restart and verify

Restart php-fpm on each server, then:

```sh
php -r 'var_dump(nocopy_shared_with());'     # "resp"
valkey-cli client list | grep -c name=nocopy-listener   # one per server
```

Each server runs one listener process (`nocopy-resp` in `ps`). It receives the server's change notifications for the whole
server and drops outdated local copies. Now write on server A and read on server B:

```sh
php -r 'nocopy_put("probe", gethostname());'      # on A
php -r 'var_dump(nocopy_get("probe"));'           # on B: A's hostname
```

## 3. Give RESP a memory limit

Set `maxmemory` and an eviction policy in `valkey.conf` (`redis.conf` on Redis). A key the server evicts is a miss on every server. For the Drupal
module, use `maxmemory-policy volatile-lfu` ([why](../../drupal/README.md#several-web-servers)).

## Use TLS

```ini
nocopy.resp=tls://user:secret@resp.example.com:6380    ; or rediss://
nocopy.resp_tls_ca_file=/etc/ssl/resp-ca.pem             ; omit to use the system store
; mutual TLS, if the server sets tls-auth-clients:
nocopy.resp_tls_cert_file=/etc/ssl/nocopy.crt
nocopy.resp_tls_key_file=/etc/ssl/nocopy.key
```

nocopy uses TLS 1.2 or newer and checks the certificate against the host name (or IP), with SNI. php-fpm's master reads
the files once at startup, so the key file can be readable by root only. Set `nocopy.resp_tls_verify=0` only in tests.
A build without OpenSSL says so on the first connection.

## When the server is down

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
- **Security:** trust the RESP server as much as php-fpm. Values read from it are unserialized with every class allowed (as in
  APCu or Symfony's Redis adapter). Use AUTH/ACLs and TLS, and do not share the key prefix with untrusted clients.
- **Clocks:** TTLs go to the RESP server as relative times, so the server's clock does not matter. Local copies expire at the time the
  writer computed, so run NTP: servers whose clocks differ by more than a TTL keep copies for too short or too long.
- **Not supported yet:** Sentinel, Cluster, TLS over unix sockets.

Background: [Sharing: by name and through RESP](../explanation/sharing.md).
