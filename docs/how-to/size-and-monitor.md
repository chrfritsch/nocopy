# Size and monitor the cache

## Pick `nocopy.shm_size`

Start with what your data needs after warm-up, plus headroom: run your site, then check

```php
$s = nocopy_stats();
printf("%.1f of %.1f MB used, %d entries\n", $s['used'] / 1e6, $s['size'] / 1e6, $s['entries']);
```

The 2,000-node Drupal benchmark site used 11 MB with every cache bin in nocopy. The whole size is reserved in `/dev/shm`
when the cache is created (raise Docker's `--shm-size` to match).

Size also decides the shard count, and an entry must fit into one shard:

| `nocopy.shm_size` | shards | largest entry (measured) |
|---|---|---|
| 8M | 1 | 7 MB |
| 16M | 2 | 7.5 MB |
| 32M (default) | 4 | 7 MB |
| 64M | 8 | 7 MB |
| 128M | 8 | 12 MB |

Roughly: shards double with size up to 8, each at least 8 MB; an entry gets a bit less than `shm_size / shards`.

More shards mean less write contention: each shard has its own lock.

## Many small entries: raise `nocopy.entries_hint`

The slot table is fixed at startup: about 2 slots per `nocopy.entries_hint`, at least one per 256 bytes of arena, and a shard
fills at most 3/4 of its slots. With many entries under ~300 bytes the cache runs out of slots before memory and evicts early.
Signs: `entries` stays near 3/4 of `slots` while `used` is well below `size`, and `evicted` grows. Then set
`nocopy.entries_hint` to the number of entries you expect.

## Watch it in production

The numbers that tell you something:

| Look at | Healthy | If not |
|---|---|---|
| `hits / (hits + misses)` | high, stable | cache too small, TTLs too short, or keys that never repeat |
| `evicted` growing | slow or flat | raise `nocopy.shm_size` (or `entries_hint`, see above) |
| `expunges` | rare | each one is a round of eviction under memory pressure |
| `lock_steals` | 0 | a worker died while holding a lock (kill -9, OOM killer); repaired, but find out why |
| `valkey_listener` | a pid, with `nocopy.valkey` | 0: no listener, nothing local is served |

All keys: [`nocopy_stats()` reference](../reference/stats.md). Counters are per arena, since it was created.

A tiny endpoint for your monitoring:

```php
<?php // stats.php, protect it
header('Content-Type: application/json');
echo json_encode(nocopy_stats() + ['shared_with' => nocopy_shared_with()]);
```

## Copy mode memory

CLI scripts and FrankenPHP workers fetch copies, and keep copies of values fetched twice for reuse, up to `nocopy.local_size`
(8M per process, counted against `memory_limit`). Lower it for many parallel CLI workers with a tight `memory_limit`,
`0` turns it off.
