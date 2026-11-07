# Size and monitor the cache

## Pick `nocopy.shm_size`

Start with what your data needs after warm-up, plus headroom: run your site, then check

```php
$s = nocopy_stats();
printf("%.1f of %.1f MB used, %d entries\n", $s['used'] / 1e6, $s['size'] / 1e6, $s['entries']);
```

The 2,000-node Drupal benchmark site used 11 MB with every cache bin in nocopy. The whole size is reserved in `/dev/shm`
when the cache is created (raise Docker's `--shm-size` to match).

Size also decides the shard count, and an entry must fit into one shard's share (shards borrow free memory from each other, but a single entry does not):

| `nocopy.shm_size` | shards | largest entry (measured) |
|---|---|---|
| 8M | 1 | 7 MB |
| 16M | 2 | 7.5 MB |
| 32M (default) | 4 | 7 MB |
| 64M | 8 | 7 MB |
| 128M | 8 | 12 MB |

Roughly: shards double with size up to 8, each at least 8 MB; an entry gets a bit less than `shm_size / shards`.

More shards mean less write contention: each shard has its own lock.

## Too small? Compare `ghost_hits` with `evicted`

`evicted` alone does not say whether more memory would help: evicting keys that are never asked for again costs nothing.
`ghost_hits` counts evicted keys that came back soon after (the cache remembers a tag of each evicted key), so

```php
$s = nocopy_stats();
printf("%.0f%% of evicted keys came back, %.1f%% of misses were such keys\n",
    100 * $s['ghost_hits'] / max(1, $s['evicted']), 100 * $s['ghost_hits'] / max(1, $s['misses']));
```

- **Most evicted keys come back** (`bench/hitratio.php`, Zipf over 1.5x the arena: 82%): the working set does not fit,
  raise `nocopy.shm_size`. The share of misses that were such keys is roughly what more memory can win back.
- **Few come back** (same with one-off keys mixed in: 13%): evictions are keys used once; more memory buys little.
  Shorter TTLs on those keys, or not caching them, helps more.

It is an estimate: tags are kept for about as many recent evictions as the cache has slots, and colliding tags overwrite each other.

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
| `ghost_hits / evicted` | low | most evicted keys come back: raise `nocopy.shm_size` ([above](#too-small-compare-ghost_hits-with-evicted)) |
| `expunges` | rare | each one is a round of eviction under memory pressure |
| `lock_steals` | 0 | a worker died while holding a lock (kill -9, OOM killer); repaired, but find out why |
| `valkey_listener` | a pid, with `nocopy.valkey` | 0: no listener, nothing local is served |

All keys: [`nocopy_stats()` reference](../reference/stats.md). Counters are per arena, since it was created.

From a shell, with [`cli/nocopy`](../../cli/nocopy) (`chrfritsch/nocopy-cli`; run as the pool's user, with the pool's
`nocopy.shm_name`, see [Share between php-fpm and the CLI](share-between-fpm-and-cli.md)):

```sh
sudo -u www-data php cli/nocopy stats          # or --json
sudo -u www-data php cli/nocopy keys user.     # key, size, age, TTL left
sudo -u www-data php cli/nocopy get user.42
sudo -u www-data php cli/nocopy delete user.42 user.43
sudo -u www-data php cli/nocopy clear user.    # no prefix: the whole cache
```

Unlike APCu (and `cachetool`), no FastCGI round trip: the CLI attaches to the same memory as php-fpm.

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
