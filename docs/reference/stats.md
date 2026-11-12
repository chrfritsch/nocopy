# `nocopy_stats()`

Returns an array that describes the whole cache, for all processes together, not just the one calling it. Counters
count since the cache was created.

- Without a shared cache, every number is `0`, `shm_name` is `null`, and `off` says why.
- With `nocopy.resp`, the numbers describe this server's local copies only.

What to watch in production: [Size and monitor the cache](../how-to/size-and-monitor.md).
Words like segment, shard and slot: [Glossary](glossary.md).

## Is it working?

| Key | Meaning |
|---|---|
| `off` | `null` while this process (or thread) has the cache. Otherwise the reason it does not, for example `nocopy.shm_name is not set (e.g. /nocopy)` or `no worker slot: …`. Then reads miss, and writes and `nocopy_clear()` return `false` |
| `shm_name` | Name of the shared-memory object in `/dev/shm`: `nocopy.shm_name`, a dot and the build id. `null` without a cache |
| `resp_listener` | Pid of this server's RESP listener process, `0` if there is none |
| `lock_steals` | Locks taken over from a process that died while holding one (its part of the cache was then repaired). Should be `0` |

## Traffic

| Key | Meaning |
|---|---|
| `hits`, `misses` | Reads that found / did not find their key, by every process. Hit rate: `hits / (hits + misses)` |
| `entries` | Entries in the cache, including expired ones not removed yet (removal happens during writes) |
| `leases` | `nocopy_remember()` computations running right now |

## Memory

| Key | Meaning |
|---|---|
| `size` | Cache size in bytes (the creator's `nocopy.shm_size`) |
| `used` | Bytes held by entries, including expired ones not removed yet |
| `slots` | Size of the slot table that finds keys. A shard fills at most 3/4 of its slots |
| `shards` | Independent parts of the cache, each with its own lock (1-8) |

## Eviction

| Key | Meaning |
|---|---|
| `evicted` | Entries removed to make room |
| `expired` | Entries removed because their TTL passed |
| `ghost_hits` | Keys written again shortly after they were evicted. They get extra protection from eviction. Compare with `evicted` to see whether more memory would help |
| `expunges` | Eviction rounds. Each merges up to 4 old segments: entries that were read move forward, the rest go |
| `evacuated_bytes` | Bytes moved by compaction and eviction rounds |

## Segments

The cache stores entries in fixed-size blocks called segments.

| Key | Meaning |
|---|---|
| `segment_size` | Bytes per segment: a power of two, 64 KB to 4 MB. Caches over 1G get more segments, not bigger ones |
| `segments` | Number of segments |
| `free_segments` | Segments that are empty and ready for any shard |
| `dead_segments` | Segments without live entries that cannot be reused yet (a reader may still use them) |
| `pinned_segments` | Segments some process is reading from right now |
| `kept_segments` | Of those, segments that web requests keep until they end. Above a quarter of `segments`, requests copy what they fetch from then on |

## Shared strings and arrays

Short strings and equal arrays that many entries contain (cache tags, render keys, metadata) are stored once for the whole
cache, and entries point at them. This space is `size / 16`, split into two halves that take turns: when the current half
is full, the older half is emptied and reused, but only once no entry still uses it.

| Key | Meaning |
|---|---|
| `shared_strings` | Strings (up to 64 bytes) stored once, in the current half |
| `shared_arrays` | Arrays (128 bytes or more, no objects inside) stored once, in the current half |
| `shared_strings_used` | Bytes the shared strings and arrays take, in both halves |
| `shared_arrays_used` | Bytes the shared arrays take, in both halves (included in `shared_strings_used`) |
| `shared_strings_size` | Bytes reserved for both halves together |
| `shared_strings_rotations` | How often the halves have switched |
