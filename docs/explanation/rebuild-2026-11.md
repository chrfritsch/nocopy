# If nocopy were rebuilt from the start, 2026-11-10

The [design review](design-review-2026-11.md) lists what to patch. This page asks the other question: which decisions would
be taken differently on an empty repository, because they are cheap at the start and expensive to retrofit. It is a
snapshot and an opinion, written from the docs and the code by reading.

**Nothing here is measured.** By the project's rule a performance claim needs numbers, so every item is a proposal until
`./dev ab` and the tests named with it have run. What gets built moves to [Design decisions](design-decisions.md) with its
numbers; what is measured and dropped goes into its table.

## What would change

Ordered by how much of the design review each one removes.

### 1. Liveness from the kernel, not from `/proc`

**Built 2026-11-10** ([design decisions](design-decisions.md#sharing)): owner ids are a process slot and its generation, the
slot held by an OFD lock; test 095 passes. Departures from the plan below: a process slot table of its own, not the worker
slots (ZTS threads each claim a worker slot, liveness is per process); macOS keeps pid + start time from `sysctl`, which
needs neither a descriptor nor `/proc`.

Today a holder is its pid and start time, and "dead" is whatever `/proc/<pid>/stat` fails to confirm (`arena/proc.c`):
another PID namespace, a process at its fd limit or a missing `/proc` all read as dead, and dead lets the caller steal a
lock, a worker slot, a lease ([finding 1](design-review-2026-11.md#1-liveness-is-a-guess-that-fails-toward-dead)).

From the start: each process takes a kernel file lock on its worker slot's byte of the shm object (an open-file-description
byte-range lock on a descriptor of its own, since forks share a description), and a lock word holds the slot index instead
of an owner id. "Is the holder dead?" is then a lock query on the object every process already has open:

- the kernel drops the lock when the process dies, so there is no pid reuse and no start time to compare;
- the check needs no new descriptor, and the answer is the same in every PID namespace;
- a process that cannot get its descriptor learns so when it claims its slot, where the outcome is "no cache", not later.

Price: one more descriptor per process; ZTS threads share their process's liveness, as they share its owner id today. The
query runs only on the slow path (a waiter that has yielded ~1000 times, a blocked reclaim, a held lease), so no fast
path changes. To verify: that macOS has these locks (`F_OFD_SETLK`; else `flock` on a
per-slot file there), and `tests/c/crash.c` and `repair.c` passing unchanged.

### 2. A pin budget for the arena, and the fetch mode by behaviour

**Built 2026-11-10** ([design decisions](design-decisions.md#memory-lifetime)): the budget counts the union of live workers'
kept segments (published bitmaps), not a sum, which would count each hot segment once per request; requests older than
`nocopy.zero_copy_seconds` (3) copy. Four long requests: other workers' hit ratio 49% -> 81%; eight: 36% -> 51-55%, the rest
held by idle pins of sleeping requests. Tests 096, 097.

The cap is a quarter of the segments per request, and zero-copy is chosen by SAPI name. Four long php-fpm requests took the
other workers' hit ratio from 100% to 27% ([finding 2](design-review-2026-11.md#2-the-pin-cap-is-per-request-not-per-arena)).

From the start: kept segments are counted arena-wide (one atomic add per newly kept segment, where `nocopy_keep` already
runs), and a request copies from the moment the arena's budget is spent or the request is older than a few seconds. The
SAPI name stays as the first guess; it stops being the only one.

### 3. One degraded state with one behaviour

**Built 2026-11-10**: off covers no name, no attach and no worker slot (also a forked child without a liveness lock, which
threw before); the route is per thread. `nocopy_clear()` is `false` when off, and also when Valkey cannot be reached.
`nocopy_stats()['off']` carries the reason, and the console prints it and exits 2. Ignored size or slot settings warn at
attach. Tests 058 (clear), 098, `tests/c/repair.c` (no descriptor, no free process slot). Not done: naming the owner of a
foreign object (finding 3).

No arena, no worker slot, an object owned by someone else and settings that differ from the creator's behave in four ways
today: misses, `nocopy_clear()` answering `true`, an uncaught `Error`, silence (findings
[3](design-review-2026-11.md#3-no-cache-is-silent-exactly-where-it-matters),
[9](design-review-2026-11.md#9-1024-worker-slots-then-every-call-throws) and
[10](design-review-2026-11.md#10-fixed-at-creation-settings-are-ignored-silently-on-attach)).

From the start there is one state, "off", with one contract: every read misses, every write returns `false` (`clear`
included), `nocopy_stats()` carries the reason as a string, and the console exits non-zero. A setting the arena ignores is
a warning at attach. The all-or-nothing rule stays as it is: a private fallback would still be worse.

### 4. Identity and address fixed when built and when created

- A build id compiled into the binary, not a hash of the file `dladdr` names when the arena is opened: that file can have
  been replaced since the code was loaded, and one fpm pool then runs two caches
  ([finding 4](design-review-2026-11.md#4-the-layout-id-is-read-from-disk-when-the-arena-is-opened)). It also returns the
  0.24-0.5 ms a CLI start pays for the hash.
- A base address chosen at random by the creator and stored in the header. Attachers already map at the header's base, so
  the fixed `0x2000'0000'0000` buys nothing and gives every host the same pointers
  ([finding 7](design-review-2026-11.md#7-a-named-file-full-of-pointers-at-a-fixed-address)).

### 5. A monotonic clock locally, relative TTLs to Valkey

The arena does not survive a boot, so a boot-relative clock (`CLOCK_BOOTTIME`, or the coarse monotonic one where its cost
matters) can carry expiry, leases and `seq`: a stepped wall clock then neither expires entries early nor keeps them late.
Towards Valkey, `PX` instead of `PXAT`, so no two servers' clocks are compared
([finding 6](design-review-2026-11.md#6-valkey-ttls-use-the-writers-wall-clock)). To measure: the coarse monotonic read
against the 3.5 ns of `CLOCK_REALTIME_COARSE`; `created` in `nocopy_stats()` and the Valkey payload still need wall time.

### 6. The Valkey tier later, apart, and with its contract written first

It is the largest unit (1,212 lines, plus ~7k vendored), starts a daemon out of a PHP extension and holds two of the
review's six real flaws. What a reader may rely on had to be worked out afterwards
([finding 5](design-review-2026-11.md#5-valkey-a-process-that-starts-no-new-request-never-re-checks)).

From the start: the sentence about staleness comes before the code, and it bounds staleness by time, not by request; the
core ships and is tested without the tier (the `nocopy_detour` seam and `./dev novalkey` already go there); the listener is
designed as a process someone can supervise, with being spawned by a worker as the convenience, not the only way.

### 7. The test harness before the features

The audit's five bugs were invisible to `./dev test`, `stress 30` and `fuzz 20000`, and the assertion that a write fills
what was planned was compiled out of every image ([audit](audit-2026-11.md#tooling-added)). On an empty repository the
arena starts as the standalone library it became (`arena/`), and CI has from the first commit: kills at named steps
(`AR_HOOK`, `tests/c/crash.c`), `block_check` after every write, a run on a debug PHP, and a fuzzer that writes to what it
fetched.

### 8. The engine's layout assumptions in one place, with an upper version guard

About 80 lines assume `HashTable`, refcount, type-info and object layouts
([finding 11](design-review-2026-11.md#11-engine-internals-without-an-upper-version-guard)). From the start they sit in one
unit with static assertions, and a PHP minor above the last one CI runs fails the build unless overridden: a new minor is
a decision.

### 9. Atoms last, or not at all

A judgement, not a finding. Atoms save 16% of a warmed Drupal's bytes and a third of the misses at 32M
([design decisions](design-decisions.md#writes)), and they bring the arena's most intricate protocol: generations, views,
per-segment bits, a rotation that takes every shard lock. A larger `nocopy.shm_size` buys a similar hit ratio with no
code. In a rebuild they come after everything above, and only if a deployment that cannot raise its size asks.

## What would stay

- The segment log with merge as compaction and eviction in one.
- Lock-free reads: pin, re-check, then dereference.
- Planning decides, writing replays the tape.
- Fixed slot tables, the mixed slot hash, the unseeded key hash (measured).
- Owner-held spinlocks rather than robust mutexes (measured); item 1 changes how a dead holder is found, not the lock.
- Two fetch modes: kept copies do not outlive a request, so php-fpm needs zero-copy and workers need copies.
- All or nothing: no private fallback cache.
- Layers checked by `./dev layers`, and the decision log with numbers. The log is why this list is short.
