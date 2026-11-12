# Size and monitor the cache

This guide helps you pick `nocopy.shm_size`, tell whether the cache is too small, and watch it in production.
Terms like shard and slot are explained in the [glossary](../reference/glossary.md).

## Pick `nocopy.shm_size`

Measure what your data needs: run your site until the cache is warm, then check

```php
$s = nocopy_stats();
printf("%.1f of %.1f MB used, %d entries\n", $s['used'] / 1e6, $s['size'] / 1e6, $s['entries']);
```

and add headroom. For comparison: a 2,000-node Drupal site with every cache bin in nocopy used 11 MB.

The whole size is reserved in `/dev/shm` when the cache is created, so the machine (or Docker's `--shm-size`) must have
that much room.

### The largest entry you can store

The cache is split into up to 8 shards, each with its own lock, so writers rarely wait for each other. Shards share free
memory, but a single entry must fit into one shard:

| `nocopy.shm_size` | shards | largest entry (measured) |
|---|---|---|
| 8M | 1 | 7 MB |
| 16M | 2 | 7.5 MB |
| 32M (default) | 4 | 7 MB |
| 64M | 8 | 7 MB |
| 128M | 8 | 12 MB |

Rule of thumb: shards double with size up to 8, each at least 8 MB, and an entry can be a bit less than
`shm_size / shards`. `nocopy_put()` returns `false` for anything larger.

### Space for shared strings

`shm_size / 16` is set aside for short strings and arrays that many entries have in common (cache tags, render keys,
`#cache` metadata, page chrome). They are stored once and the entries point at them. On a warmed Drupal site they take
1.3 MB and save 29% of the entries' size.

If `shared_strings_used` is close to `shared_strings_size` and `shared_strings_rotations` stops going up, that space is
full of strings that old entries still use. New entries then store their own copies until the old ones are gone. Nothing
breaks; a larger `shm_size` gives this space more room.

## Is it too small? Compare `ghost_hits` with `evicted`

A growing `evicted` count alone does not mean you need more memory: evicting keys that nobody asks for again costs
nothing. What matters is whether evicted keys **come back**. The cache remembers a small trace of each evicted key and
counts the ones that are written again soon after in `ghost_hits`:

```php
$s = nocopy_stats();
printf("%.0f%% of evicted keys came back, %.1f%% of misses were such keys\n",
    100 * $s['ghost_hits'] / max(1, $s['evicted']), 100 * $s['ghost_hits'] / max(1, $s['misses']));
```

- **Most evicted keys come back** (`bench/hitratio.php` with 1.5x more data than memory: 82%): your working set does not fit.
  Raise `nocopy.shm_size`. The share of misses that were such keys is roughly what more memory can win back.
- **Few come back** (the same benchmark with one-off keys mixed in: 13%): the evicted keys were used once. More memory buys
  little; shorter TTLs on those keys, or not caching them at all, helps more.

This is an estimate: traces are kept for about as many recent evictions as the cache has slots, and two keys can share a
trace.

## Many small entries: raise `nocopy.entries_hint`

Keys are found through a slot table whose size is fixed at startup: about 2 slots per `nocopy.entries_hint`, and at least
one slot per 256 bytes of cache. A shard uses at most 3/4 of its slots. If you store many entries under ~300 bytes, the
cache can run out of slots before it runs out of memory, and starts evicting early.

You are in this situation when `entries` stays near 3/4 of `slots`, `used` is well below `size`, and `evicted` grows.
Set `nocopy.entries_hint` to the number of entries you expect.

## Watch it in production

The numbers that tell you something ([all keys](../reference/stats.md)):

| Look at | Healthy | If not |
|---|---|---|
| `off` | `null` | This process has no cache; the value says why |
| `hits / (hits + misses)` | high and stable | Cache too small, TTLs too short, or keys that never repeat |
| `evicted` | grows slowly or not at all | Raise `nocopy.shm_size` (or `entries_hint`, see above) |
| `ghost_hits / evicted` | low | Most evicted keys come back: raise `nocopy.shm_size` ([above](#is-it-too-small-compare-ghost_hits-with-evicted)) |
| `expunges` | rare | Each one is a round of eviction because memory is short |
| `kept_segments` | well under `segments / 4` | Long web requests (a slow database, exports) hold memory, and their later reads copy. Lower `nocopy.zero_copy_seconds`, or cap them with `request_terminate_timeout` |
| `lock_steals` | `0` | A worker died while holding a lock (`kill -9`, OOM killer). The cache was repaired, but find out why |
| `valkey_listener` | a pid, when you use `nocopy.valkey` | `0`: no listener, so nothing is served locally |

Counters are for the whole cache and count since it was created.

### From a shell

The console [`cli/nocopy`](../../cli/nocopy) (`chrfritsch/nocopy-cli`) works like cachetool. Run it as the pool's user,
with the pool's `nocopy.shm_name` ([why](share-between-fpm-and-cli.md)):

```sh
sudo -u www-data php cli/nocopy stats          # or --json
sudo -u www-data php cli/nocopy keys user.     # key, size, age, TTL left
sudo -u www-data php cli/nocopy get user.42
sudo -u www-data php cli/nocopy delete user.42 user.43
sudo -u www-data php cli/nocopy clear user.    # no prefix: the whole cache
```

Unlike with APCu and cachetool, there is no FastCGI round trip: the CLI opens the same shared memory as php-fpm.

### From your monitoring

A tiny endpoint (protect it):

```php
<?php // stats.php
header('Content-Type: application/json');
echo json_encode(nocopy_stats() + ['shared_with' => nocopy_shared_with()]);
```

## Memory of CLI scripts and workers

CLI scripts and FrankenPHP workers get private copies, and keep copies of values they read twice so they can reuse them.
Those kept copies are limited by `nocopy.local_size` (8M per process by default) and count against `memory_limit`. Lower
it if you run many CLI workers in parallel with a tight `memory_limit`; `0` turns it off.
