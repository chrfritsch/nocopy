# Valkey as nocopy's shared tier

## Why
nocopy alone is per server: every server has its own cache, an fpm restart empties it (first logged-in Drupal request 476-544 ms),
and CLI writes need the database journal. Real sites run several web servers on a shared Valkey. Goal: Valkey is the cache of record,
nocopy stays the per-server zero-copy L1 in front of it, invalidated by Valkey. No phpredis: nocopy.so speaks RESP3 itself (Relay model).

## Decided
- `nocopy.valkey` (INI, off when empty): `tcp://[user:pass@]host:port` or `unix:///path`. `nocopy.valkey_prefix` (default `nocopy:`)
  namespaces the keys in Valkey. `nocopy.valkey_timeout` (s, default 0.5): connect and reply timeout.
- One connection per worker (process / ZTS thread), opened lazily, kept across requests. `HELLO 3`, then
  `CLIENT TRACKING ON BCAST PREFIX <prefix> NOLOOP`: every connection is told about every change under the prefix made by other connections
  (writes, deletes, expiry, eviction in Valkey; `FLUSHALL` -> clear).
- Pushes are applied (local delete) at the first nocopy call of each request and whenever a reply is read. All workers of a server receive
  them, so an idle worker never leaves the shared arena stale.
- Read: local hit as today. Local miss: `GET`, decode, store locally (with the guard below), return.
- Write (`nocopy_put`, `apcu_store/add`, remember/entry results): validate, `SET [NX] [PXAT]` in Valkey, then store locally.
  Delete: `UNLINK` + local drop. `apcu_inc/dec`: `INCRBY` in Valkey (created with the TTL if missing), local copy dropped.
  `apcu_cas`: a small Lua script. `apcu_clear_cache`: `SCAN`+`UNLINK` over the prefix, then local clear.
- Encoding in Valkey: integers as plain decimal (so `INCRBY` works); everything else `0x00, flags, int64 expires, uint32 compute ms, payload`
  with payload = `nocopy.serializer` (php or igbinary).
- Fill guard: a shared invalidation counter in the arena, bumped before a worker applies pushes. A local store of data read from Valkey
  is dropped again if the counter moved since before the Valkey round trip (an invalidation for it may have been applied in between).
  ponytail: one global counter; per-shard counters if heavy write traffic makes fills drop too often.
- Valkey unreachable: nocopy serves no local data (no invalidations = possibly stale), fetches miss, writes return false.
  Reconnect after a lost connection clears the local arena. Retried at most once a second per worker.
- Local-only: `APCUIterator`, `apcu_cache_info`, `nocopy_stats`, leases (single-flight per server, not across servers).
- Drupal module: with `nocopy.valkey` set, no journal (CLI writes reach fpm through Valkey).

## Phase 2: optional Valkey module
Works without it (managed Valkey cannot load modules). Where loaded (detected at connect), it replaces round trips: INCR with TTL on create
and CAS without Lua, a server-side prefix clear instead of SCAN+UNLINK, maybe Drupal tag checksums. The client keeps every Valkey
command in a few functions in `nocopy_valkey.c`, so the module only swaps their bodies.

## Ceilings
- BCAST: every write is pushed to every worker connection of every server (servers x workers messages). Fine for cache traffic, not for
  write-heavy counters.
- A worker connecting fresh while all others of its server lost their connection unnoticed may read stale data until one of them notices.
- Each connection subscribes to `__nocopy_keepalive__` (exempt from Valkey's idle `timeout`), so it counts as pubsub for
  `client-output-buffer-limit` (default 32mb, or 8mb for 60 s of unread pushes): a worker idle through that many invalidations is disconnected.
- Long CLI processes that only hit locally see pushes at their next Valkey round trip.
