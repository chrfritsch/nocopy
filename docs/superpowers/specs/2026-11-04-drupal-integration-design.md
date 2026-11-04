# Drupal integration — design

## Intent

The README shows micro-benchmarks and one Symfony page. To convince people, we need gains on a real application.
The target is **Drupal on a single web server**. nocopy becomes the **cache backend for all bins**, taking the database out of
caching entirely, with cache tags kept in shared memory too. The proof is a benchmark against stock Drupal (database + APCu)
for anonymous, logged-in and editor traffic.

## Measurements behind this (spikes, 2026-11-04)

Drupal 11.4, standard recipe, 500 nodes, single server, SQLite, php-fpm. Scripts are in the session scratchpad, and the site is
in the Docker volume `nocopy-drupal-site`.

**Read cost in the fast bins** (`bootstrap`, `config`, `discovery`): time spent rebuilding values on read, averaged over
anonymous and logged-in requests (±15% between runs).

| Setup | µs/request |
|---|---:|
| APCu, PHP serializer (core `ApcuBackend` stores a `stdClass`, so everything is serialized) | 335 |
| APCu + igbinary | 180 |
| nocopy storing plain arrays, PHP serializer for values with objects | 242 |
| the same + igbinary | 124 |
| the same + object rebuild (phase 3, modeled at 100 ns/object) | ~60 |

- The cost sits mostly in objects (`entity_type`, `entity_type_definitions.installed`, `element_info_build`, field definitions).
  The same object instance often appears more than once within a value.
- Serializing only the objects and sharing the arrays around them saves almost nothing. Dropped.

**Editor loop** (edit form → save → view → `/admin/content` → add form, 50 saves):

| Step | Page | Fast bins (APCu) | DB cache reads | DB cache writes |
|---|---:|---|---|---|
| edit form | 35 ms | 95 × = 2.2 ms | 30 × = 1.1 ms | 7.7 ms |
| save (POST) | 34 ms | 57 × = 1.6 ms | 13 × = 0.5 ms | – |
| view after save | 36 ms | 83 × = 1.8 ms | 34 × = 1.0 ms | 13.4 ms |
| admin/content | 59 ms | 99 × = 1.7 ms | 111 × = 1.9 ms | 6.1 ms |
| add form | 26 ms | 76 × = 1.8 ms | 29 × = 0.9 ms | – |

- The fast bins always hit, and node saves never write to them.
- The cost is in the database bins (`render`, `page`, `dynamic_page_cache`, `entity`, `data`, …): reads plus 6–13 ms of
  writes after a save. SQLite exaggerates the writes, and MariaDB will be lower, but each write is still a query.
- Swapping APCu for nocopy behind core's `ApcuBackend` gains nothing, because core's backend makes everything serialized.

Conclusion: replacing APCu in the fast layer alone gains 3–7% on editor pages. The gain is in moving all bins, and the tag checksums,
off the database.

## Order of work

### Phase 1 — igbinary serializer (extension)

- New INI `nocopy.serializer` = `php` (default) | `igbinary`, alias `apc.serializer` (the same alias rules as the other `apc.*` settings).
  `igbinary` without the igbinary extension loaded: startup warning, fall back to `php`.
- Each entry records which serializer wrote it (a flag next to `ENTRY_SERIALIZED`), so changing the INI setting never misreads old entries.
- Applies everywhere the extension serializes: `nocopy_put(..., serialize: true)`, the `apcu_*` layer, `nocopy_remember`.
- Tests: phpt round-trip with igbinary (skipped without it), plus the APCu suite with `apc.serializer=igbinary`. The Docker image gains igbinary.
- Needed for APCu compatibility anyway: sites with `apc.serializer=igbinary` expect it to keep working.

### Phase 2 — Drupal module + benchmark

**Module:** `drupal/` next to `symfony/`, Composer `chrfritsch/nocopy-drupal`, Drupal module `nocopy`, Drupal 10.3+ / 11.
It uses `nocopy_*` only, so it also works with `nocopy.apcu_compat=0`.

**Cache backend:** `NocopyBackend implements CacheBackendInterface`, plus `NocopyBackendFactory` as `cache.backend.nocopy`.

- **Enabling it** takes `$settings['cache']['default'] = 'cache.backend.nocopy'`. That covers all bins, including those core
  wires to `ChainedFastBackend`: with the default backend set, chainedfast's consistent backend becomes nocopy and its fast
  backend (APCu) would only duplicate it. The README therefore also sets those three bins to `cache.backend.nocopy` directly.
  The `container` bin is documented separately (`bootstrap_container_definition`), as the Redis module does.
- **Key:** `Settings::getApcuPrefix('nocopy_backend', ...) . '::' . $bin . '::' . $cid`.
- **Stored value:** a plain array (`[data, created, expire, tags, checksum]`) so values without objects are zero-copy.
  Values with objects fall back to `$serialize: true`, which uses the phase 1 serializer and becomes zero-copy in phase 3.
- **Semantics** identical to core backends: `$allow_invalid` returns invalidated and expired items; expire is checked on read
  against the request time; the nocopy TTL is used only as a memory hint (expire + slack), never for validity.
- **`invalidate*()`:** rewrite the item with `expire = requestTime - 1`, as core does.
- **`deleteAll()`, `removeBin()`, `invalidateAll()`:** bump a per-bin version folded into the key prefix (the same trick as
  the Symfony adapter's `clear()`). Old entries leave through eviction.
- **Also usable as a fast layer:** the same backend works as `ChainedFastBackend`'s fast backend in front of the database,
  for people who want the database to stay the cache of record. Documented and benchmarked, not the recommended setup.

**Cache tags:** `NocopyCacheTagsChecksum implements CacheTagsChecksumInterface, CacheTagsInvalidatorInterface`, replacing
`cache_tags.invalidator.checksum` (the same service swap the Redis module documents). Tag invalidation counts live in nocopy,
so tag checks and invalidations never touch the database.

- **Invariant: a counter value is never reused.** Invalidating a tag sets its counter to the next value of one global nocopy
  sequence, so each tag's counter only grows and never returns to an earlier value.
- **A missing counter makes the item invalid.** That covers eviction and restarts. A counter that is recreated starts from the
  global sequence.
- Together these make the checksum (the sum of counters) equal only when no tag was invalidated, so eviction can never make a
  stale item look valid. Needs a dedicated test.
- The usual per-request static cache and preloading (`CacheTagsChecksumPreloadInterface`) apply, as in core's checksum service.
- Works even if some bins stay on the database: their items fail the checksum after a restart and get rebuilt, so nothing stale
  survives.

**Trade-offs (README, plainly):**

- **Restarts:** the cache is empty after a php-fpm restart, so the first requests rebuild it. The benchmark measures how long that takes.
- **Memory:** `nocopy.shm_size` must hold the render cache. When it fills up, nocopy evicts entries (misses, not errors).
  The README gives sizing guidance from the benchmark's `nocopy_stats()['used']`.
- **Multiple web servers:** not supported. Each server would have its own cache and its own tag counters.

**Tests:** kernel tests extending core's `GenericCacheBackendUnitTestBase` against `NocopyBackend`. Tag checksum tests cover
invalidation, the missing-counter and eviction cases, and counters never being reused. They run in Docker, in copy mode and with
`nocopy.request_scope=1`.

**Benchmark** (`docker/drupal-bench.sh` + `bench/drupal/`), in the style of `docker/fpm-bench.sh`:

- php-fpm (static workers) + nginx + **MariaDB** in Docker, Drupal 11 with the `standard` recipe, content via `devel_generate`.
  (Drupal 11.4's standard profile alone creates no content types, so the `standard` recipe must be applied.)
- Configs:
  1. stock: database + APCu `ChainedFastBackend` (PHP serializer)
  2. stock with APCu + igbinary
  3. nocopy as the fast layer only, database behind it (+ igbinary)
  4. **nocopy for all bins + nocopy tag checksums** (+ igbinary)
- Traffic, side by side:
  - **anonymous:** page cache on; the page bin now lives in nocopy, so anonymous hits gain too
  - **logged-in:** session cookie via `ab -C`
  - **editor:** the scripted edit → save → view → list → add loop from the spike, running concurrently with reader load
- Output per traffic type: req/s, p50, p99, failures, plus database queries per request. Results go in `bench/results-drupal/`.
- **Cold start:** time and request count until throughput settles after a php-fpm restart, configs 1 and 4.
- **README:** a Drupal section (install, `settings.php` lines, trade-offs) and Drupal rows in the benchmark table.

### Phase 3 — object rebuild from shared memory (extension)

Store objects in the immutable arena as class name + property table, and rebuild them on read without serializing:

- On read: create the instance without the constructor, then point its property slots at shared arrays and strings (no copy).
  Cost scales with the number of objects, not their bytes.
- **Object identity is preserved:** an object table is built during store and rebuild, so shared instances come back shared.
- Classes with `__serialize`/`__unserialize`: store the `__serialize()` array, and hand the shared array to `__unserialize()`.
  Enums are stored by class + case name.
- Fallback to the phase 1 serializer for the whole value: `Serializable`, `__sleep`/`__wakeup`, closures, internal classes whose
  state isn't in the property table, and anything else not provably safe.
- Matters more with all bins in nocopy: render arrays are full of `Markup` objects.
- Re-run the phase 2 benchmark with a config 5 (all bins + object rebuild).
- Its own design pass before building. Layout, class lookup cost, `readonly`/typed/uninitialized/dynamic properties are left to it.

## Non-goals

- Multiple web servers, Redis integration, a per-server layer in front of a shared cache. (Explored and dropped: Relay covers that case.)
- Native tag support in the C extension, and a Symfony tag-aware adapter. Drupal tags are handled in the module.
- Serializing only the objects while keeping the arrays around them shared (measured: not worth it).
- Persisting the cache across restarts.

## Anti-goals

- A stale item that ever looks valid, after eviction, restart or invalidation.
- Changing Drupal's cache semantics compared to core backends (`$allow_invalid`, expire, invalidate, tag invalidation, `deleteAll`).
- Benchmarks that flatter nocopy: the baselines are stock and tuned (igbinary) APCu on MariaDB, configured as core configures them.
  The cold-start cost is reported, not hidden.
- Object rebuild that behaves differently from unserialize (identity, magic methods, property state).

## Left to the builder

- Exact array layout of the stored entry, and how to detect the object fallback cheaply (try/catch on the throw vs. a pre-scan).
- Content counts, `ab` request counts and concurrency, sized so a run takes minutes.
- `getMultiple()` as a loop of `nocopy_get()` (there is no multi-get in the native API), unless profiling says otherwise.
- MariaDB tuning: defaults plus a buffer pool that holds the site, identical across configs.
- The exact `settings.php` / `services.yml` snippet for the README, including the `container` bin.
