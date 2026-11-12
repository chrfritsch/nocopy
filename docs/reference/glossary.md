# Glossary

Words these docs use, in plain terms. The full story: [How nocopy works](../explanation/how-it-works.md).

**Arena**: The block of shared memory that holds the whole cache. One per `nocopy.shm_name` (per server with Valkey). "The cache" and "the arena" mean the same thing.

**Zero-copy mode**: How web requests read: `nocopy_get()` returns the value that sits in shared memory, without copying it. If your code modifies it, PHP makes a private copy first, as it does for any array.

**Copy mode**: How CLI scripts and FrankenPHP workers read: each read returns a private copy. Copies of values read twice are kept and reused while the entry is unchanged (`nocopy.local_size`).

**Segment**: A fixed-size block of the arena (64 KB to 4 MB) that entries are written into, one after the other. Memory is freed and reused a whole segment at a time.

**Pin**: A mark a process sets on a segment while it holds values from it. A pinned segment is not reused, so a value you fetched stays valid for your whole request, even if another process deletes the key.

**Shard**: One of up to 8 independent parts of the arena, each with its own lock. A key's hash picks its shard, so writes to different shards never wait for each other. A single entry must fit in one shard.

**Slot**: An entry in the fixed-size table that finds keys. Its size comes from `nocopy.entries_hint` and `nocopy.shm_size`.

**Eviction**: Removing entries to make room when the cache is full. nocopy removes old entries that nobody read, and keeps popular ones.

**Ghost**: A small trace of a recently evicted key. If the key comes back soon, it counts as a `ghost_hit` and is protected.

**Lease**: The right to compute a missing value in `nocopy_remember()`. Only the worker holding it computes; the others wait.

**Off**: The state of a process that has no shared cache (no `nocopy.shm_name`, no permission, too many processes). Reads miss, writes return `false`, nothing throws. `nocopy_stats()['off']` says why.

**Listener**: With Valkey, one background process per server (`nocopy-valkey` in `ps`) that receives Valkey's change notifications and drops outdated local copies.

**Polyfill**: The PHP package `chrfritsch/nocopy-apcu`, which defines the `apcu_*` functions on top of nocopy.
