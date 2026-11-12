# Design decisions

Why nocopy works the way it does, and what each choice means for you. Every decision here was measured; the numbers are
from Linux in Docker on arm64 ([benchmarks](../reference/benchmarks.md)). Contributors find every measurement and every
rejected idea in the [design log](../internals/design-decisions.md).

## Reading

### Values are handed out, not copied

**Decision:** under web servers, `nocopy_get()` returns the value that sits in shared memory, stored the way PHP's
OPcache stores immutable arrays.

**Why:** APCu copies every element of an array on every read, so the cost grows with the size of what you cache.

**For you:** a 1,000-row array reads in 12 ns instead of 200 µs. Modifying it is safe: PHP copies it first, as with any
immutable array.

### Long-running processes get copies

**Decision:** CLI scripts, queue workers and FrankenPHP workers get a private copy. Copies of values read twice are kept
and reused while the entry is unchanged.

**Why:** memory that a process still points into cannot be reused, and a process that never ends a request would hold it
forever.

**For you:** nothing to configure; the mode follows how PHP runs. Kept copies make repeated reads in a worker nearly as
cheap as zero-copy ones (1,000 rows: 68 µs for a fresh copy, 13 ns for a kept one). Give workers a `nocopy.local_size`
that holds the data they read often.

### Long web requests switch to copying

**Decision:** a web request older than `nocopy.zero_copy_seconds` (3 s), or one that would push the running requests over a
quarter of the cache, copies what it fetches from then on.

**Why:** one long request that read across a full cache once pinned every segment; the hit rate of everyone else fell
from 90% to 10%. With the limit it stayed at 88.7%.

**For you:** slow requests (exports, a slow database) cannot starve the cache. Their later reads are slower, nothing else.

## Sharing

### A cache name is required, and there is no private fallback

**Decision:** without `nocopy.shm_name`, or when the cache cannot be opened, a process has no cache at all: reads miss,
writes return `false`, `nocopy_clear()` returns `false`.

**Why:** a private cache looks like it works, but `drush cr` would clear only its own copy while php-fpm keeps serving
old data. A slow cache is better than a wrong one. And with a default name, two sites of one user would share keys.

**For you:** set the name for php-fpm *and* the CLI. Check `nocopy_shared_with()` or `nocopy_stats()['off']` if something
seems off. More in [Sharing](sharing.md#all-or-nothing).

### One user, one build

**Decision:** only processes that run as the same user, with the same `nocopy.so` build, share a cache.

**Why:** anyone who can write the cache controls what php-fpm reads, so it is readable by its owner only. And a different
build may lay out memory differently.

**For you:** run CLI commands as the pool's user. After an upgrade the cache starts empty.

### RESP: one listener per server, and nothing outdated is served

**Decision:** one background process per server receives the server's change notifications for all workers. While it is
disconnected, nothing local is served.

**Why:** with every worker listening, 16 workers meant 16 connections and 16 copies of every notification. And a local copy
whose invalidation might have been missed could be outdated.

**For you:** few connections to the RESP server. When the server is down you get misses, not stale data. Each write makes every server
fetch the value again once, the writer's included: fine for cache data, not for a counter bumped on every request.

## Storing

### Objects are stored as their properties, not serialized

**Decision:** most objects are stored as their properties and rebuilt on fetch, with the same `__sleep()`, `__wakeup()`,
`__serialize()` and `__unserialize()` calls as `serialize()` / `unserialize()`.

**Why:** unserializing is slow, and one object used to force the whole value, arrays included, to be serialized.

**For you:** one object fetches in 112 ns instead of 881 ns, 100 objects in 4.6 µs instead of 83 µs, and arrays next to
an object stay zero-copy. If a class changes its properties, the old entry is a miss, not a broken object.
Internal value classes (dates, SPL containers, `ArrayObject`, random engines) are nodes too; objects that implement only
`Serializable`, exceptions and stateful internal objects (iterators, DOM) are still serialized, as in APCu.

### Strings and arrays are shared across entries

**Decision:** short strings (up to 64 bytes) and arrays of 128 bytes or more that appear in several entries are stored once
for the whole cache, once they have been seen twice.

**Why:** caches repeat themselves: Drupal's render cache has the same cache tags, `#cache` metadata and page chrome in
thousands of entries.

**For you:** a warmed Drupal site's cache shrank by 29% (38.4 to 26.0 MB, plus 1.5 MB shared). At the same `shm_size`
that means fewer evictions: with 24 MB, the hit rate went from 89.5% to 94.6%. Reads are unchanged.

### TTLs use a clock that never jumps

**Decision:** expiry is measured with the system's monotonic clock, not wall time.

**Why:** when the wall clock is corrected (NTP, an admin, a VM resuming), entries would expire early or live too long.

**For you:** a clock change does not touch your cache. With RESP, TTLs are sent as relative times, so the server's clock does
not matter either; still run NTP on your servers.

## Keys and slots

### Keys use PHP's own hash, without a secret seed

**Decision:** keys are hashed with the hash PHP already caches in every string.

**Why:** a seeded hash would protect against crafted colliding keys, but it costs every call 1.5-2 ns (a fetch went from
10.9 to 12.6 ns). APCu has the same trade-off.

**For you:** if a key contains user input, hash it with a secret first, as shown in [Functions](../reference/functions.md).

### The slot table has a fixed size

**Decision:** the table that finds keys is sized once, from `nocopy.entries_hint` and `nocopy.shm_size`.

**Why:** a table that grows has to stop everyone while it grows.

**For you:** if you store many tiny entries, the cache can run out of slots before memory. Raise `nocopy.entries_hint`
([how to tell](../how-to/size-and-monitor.md#many-small-entries-raise-nocopyentries_hint)).

## When the cache is full

### Gradual eviction instead of a wipe

**Decision:** when memory runs out, nocopy merges its oldest segments: entries that were read move forward, the rest are
evicted. It also picks segments that were read least, and remembers recently evicted keys to protect them if they come back.

**Why:** APCu throws away the whole cache when it is full, so every worker misses at once.

**For you:** a full cache keeps working, and keeps its popular entries. Use `ghost_hits` and `evicted` to see whether more
memory would help ([Size and monitor](../how-to/size-and-monitor.md)).

### No per-entry hit counter

**Decision:** each entry has a small probabilistic read counter (0-15) instead of an exact one.

**Why:** an exact counter is a write to shared memory on every read, which slows down every worker that reads the same
key. The small counter writes about log2(reads) times.

**For you:** reads of hot keys stay fast with many workers: 8 workers read 350 million values per second.

## Stampedes

### `nocopy_remember()`: compute once, serve the old value meanwhile

**Decision:** on a miss, only one worker computes; the others wait for its result, or get the value that just expired
(if it expired less than one TTL ago). Popular values are refreshed shortly before they expire.

**Why:** when a popular entry expires, every worker misses at the same moment and recomputes it.

**For you:** with 16 workers waking up after an idle period, slow requests fell from 112 to 22. Use `nocopy_remember()`
for anything expensive to compute.

## Integrations

### APCu compatibility is a PHP polyfill

**Decision:** `apcu_*` are defined in PHP on top of `nocopy_*`, not in the extension.

**Why:** it keeps the extension small, and code you control is better off calling `nocopy_*` directly.

**For you:** require `chrfritsch/nocopy-apcu` for code you can't change; switch your own code to `nocopy_*` for full speed
([Migrate from APCu](../how-to/migrate-from-apcu.md)).

### The Symfony bundle's `get()` runs on `nocopy_remember()`

**Decision:** the bundle's `$pool->get($key, $callback)` does not use Symfony's `AbstractAdapter::get()`.

**Why:** Symfony's version wraps every value that has an expiry in an object, which forced it to be serialized.

**For you:** a hit with `expiresAfter()` takes 190 ns instead of 1,958 ns, and you get stampede protection.

### The Drupal module needs the APCu polyfill

**Decision:** the module requires `chrfritsch/nocopy-apcu`.

**Why:** Drupal's class loader and FileCache look for `apcu_fetch()`; without it they check the file system for every class.

**For you:** 807 file system checks per logged-in request become 65, and pages get 11-39% faster.

## Tried and dropped

| Idea | Why not |
|---|---|
| Compressing entries in shared memory | Every read would have to decompress (10-27 µs for a render array, against 0 now) |
| Compressing what goes to the RESP server | igbinary already makes it 2-2.3x smaller; compression pays only when RESP is short of memory or bandwidth |
| Native cache tags in C | At most 0.5-1.5% of a Drupal page-cache hit; Drupal already checks all tags in one lookup |
| A seeded key hash | 1.5-2 ns on every call; see above |
| Dropping `nocopy_get_multiple()` for a PHP loop | 10 keys: 142 ns become 292 ns |
| Dropping `nocopy.entries_hint` for a bigger fixed table | Reads of many distinct keys 22% slower, and 4.7% more memory in tables |
| More optimization of the request path | `nocopy.so` is 0.3% of a Drupal node page's CPU time; nothing left to win there |

The complete list, with all numbers: [design log](../internals/design-decisions.md#measured-and-rejected).
