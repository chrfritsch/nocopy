# `nocopy_stats()`

Returns an array describing the whole arena (all processes), not just the calling process. Counters run since the arena
was created. Without a shared cache, every number is `0` and `shm_name` is `null`.
With `nocopy.valkey`, these describe this server's local copies only.

| Key | Meaning |
|---|---|
| `size` | Arena size in bytes (`nocopy.shm_size` of the creator) |
| `used` | Bytes held by entries (also expired ones not yet removed) |
| `entries` | Live entries, including expired ones not yet removed (removal runs on writes) |
| `hits`, `misses` | Fetches that found / did not find their key, by every process |
| `evicted` | Entries removed to make room |
| `expired` | Entries removed because their TTL passed |
| `expunges` | Eviction rounds (each merges up to 4 old segments: read entries move forward, the rest go) |
| `ghost_hits` | New keys recognized as recently evicted, which then get extra protection from eviction |
| `evacuated_bytes` | Bytes moved by compaction and eviction rounds |
| `segment_size` | Bytes per segment (a power of two, 64 KB-4 MB; arenas over 1G get more segments, not bigger ones) |
| `segments` | Segments in the arena |
| `free_segments` | Segments in the shared pool, ready for any shard's writes |
| `dead_segments` | Segments without live entries, not yet reused (a reader may still pin them) |
| `pinned_segments` | Segments some process currently reads from |
| `shards` | Independent parts with their own lock (1-8) |
| `slots` | Slot table size; a shard fills at most 3/4 of its slots |
| `lock_steals` | Locks taken over from a dead process (and its shard repaired). Should be 0 |
| `leases` | `nocopy_remember()` computations in progress |
| `shm_name` | Name of the attached shared-memory object (`nocopy.shm_name` + `.` + build id), or `null` |
| `valkey_listener` | Pid of this arena's Valkey listener, `0` if none |

Hit rate: `hits / (hits + misses)`. What to watch: [Size and monitor the cache](../how-to/size-and-monitor.md).
