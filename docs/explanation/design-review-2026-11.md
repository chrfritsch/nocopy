# Adversarial design review, 2026-11-10

The [correctness audit](audit-2026-11.md) checked the code against fixed invariants. This review attacks the invariants and
the premises behind them: which design decisions hurt real deployments in ways no bug fix repairs. No C code changed in
this pass. It is a snapshot; decisions that follow from it belong in [Design decisions](design-decisions.md).

Every claim has a `file:line` and, where one was cheap, a reproduction. A claim without one is marked *by reading* or
*speculative*. Nothing measured in [Design decisions](design-decisions.md) is reopened, with one exception: the rejection
of robust mutexes compared speed only, and finding 1 shows what the replacement assumes (the speed numbers stand).

## Summary

Ranked by blast radius × likelihood in real deployments.

| # | Finding | Verdict | What the user sees | Docs admit it | Cheapest mitigation |
|---|---|---|---|---|---|
| 1 | Liveness is a guess that fails toward "dead" | real flaw | segfaults and wrong values in every process | wrongly: "cannot share" | fail closed + refuse another PID namespace at attach |
| 2 | The pin cap is per request, not per arena | real flaw | hit ratio 100% -> 27% while 4 requests run long | half | a global cap; a stat |
| 3 | "No cache" is silent exactly where it matters | real flaw | `cache:clear` clears nothing and says `true` | partly | `nocopy_clear()` false; a health check; `stats()` field |
| 4 | The layout id is read from disk when the arena is opened | real flaw | one fpm pool split over two caches; 2x `shm_size` | no | hash once at load; collect on a timer or at request end |
| 5 | Valkey: a process that starts no new request never re-checks | real flaw for workers | stale reads without bound | "by design" for a request | re-check the heartbeat by age |
| 6 | Valkey TTLs use the writer's wall clock | real flaw with skew | short TTLs never cached, or outliving their TTL | no | `PX` instead of `PXAT` |
| 7 | A named file full of pointers at a fixed address | trade-off, under-documented | a file write as the pool's user is code execution in every worker | half | docs; a random base |
| 8 | A stopped lock holder is waited for forever | trade-off, undocumented | writers spin at 100% CPU | no | docs; a spin budget |
| 9 | 1,024 worker slots, then every call throws | trade-off, undocumented | `Error: no free worker slot` | no | degrade to no cache; docs |
| 10 | Fixed-at-creation settings are ignored silently on attach | trade-off, undocumented | a raised `shm_size` does nothing | no | a warning |
| 11 | Engine internals without an upper version guard | trade-off | a new PHP minor compiles and may corrupt silently | no | `#error` above the tested minor |

Ruled out, with reasons: [below](#ruled-out).

## How this was run

Cloud sandbox, amd64, 4 vCPU, kernel 6.18, PHP 8.5. `./dev` could not build its image here (`pecl` has no route to its
channel from build containers, as in the audit), so the image was built from the same Dockerfile with `apcu` and `igbinary`
installed from downloaded tarballs, and each run used `./dev`'s own `docker run` line (`--shm-size=1g`, the source at
`/src` read-only, `docker/build.sh`). Scripts were scratch files unless a test is named; each is described closely enough
to rewrite. Timings are from a shared VM: read ratios, not absolutes.

The one committed test is `tests/095_liveness_fd_limit.phpt` (finding 1), marked `--XFAIL--`: it states the behaviour
wanted and fails today. `docker/test.sh` now accepts an expected failure's `.diff`. Full suite with it: 85 passed, 1 skipped, 1 expected failure.

## Findings

### 1. Liveness is a guess that fails toward "dead"

**Premise attacked.** "A lock held by a crashed process is detected by its pid and start time" ([Sharing](sharing.md)),
and so owner-id spinlocks plus `repair()` can replace robust mutexes.

**Evidence.** `ar_owner_dead()` (`arena/proc.c:68`) answers "dead" when `kill(pid, 0)` says `ESRCH`, or when
`proc_start()` does not return the recorded start time. `proc_start()` returns 0 for *any* failure to open or read
`/proc/<pid>/stat` (`arena/proc.c:35`, `:43`, `:48`, `:53`). Every "cannot tell" is therefore "dead", and "dead" is the
answer that lets the caller steal a shard lock and run `repair()` (`arena/proc.c:92`, `arena/shard.c:41-47`), take a
worker slot and reset its pins (`arena/proc.c:134`, `arena/arena.c:53-64`), ignore a worker's pins when freeing a segment
(`arena/pool.c:146`), replace a lease (`arena/leases.c:53`) and take the atom lock.

Three ways a live process looks dead:

- **Another PID namespace.** Nothing ties an arena to a namespace: the name is `shm_name` + a hash of `nocopy.so`
  (`arena/region.c:232`), so two containers from one image that share `/dev/shm` (`docker --ipc=container:…`, `--ipc=host`,
  a Kubernetes pod with a `medium: Memory` volume at `/dev/shm`, a sidecar running the CLI) attach to one arena. The other
  side's pids do not exist, or belong to someone else.
  Reproduced: a self-checking churn (forked CLI workers, random put/get of values that carry their own key and
  length, 16 MB), 10 s. One container, 8 workers: 0 steals, 0 crashes, 0 bad values. Two containers sharing IPC, 4 workers
  each: `lock_steals` 84, 5 workers killed by SIGSEGV, one died allocating 5063812098665369600 bytes, and 22 fetches
  returned a value that failed its own check. Copy mode; zero-copy readers also lose their pins (`pool.c:146`).
- **No file descriptor left.** A process at `RLIMIT_NOFILE` cannot open `/proc/<pid>/stat`.
  Reproduced in one PID namespace: the same churn with `ulimit -n 64` and half of the 8 workers holding every fd: 9 steals,
  4 SIGSEGV, 7 bad values in 10 s. Deterministic through the lease path: `tests/095_liveness_fd_limit.phpt` (a live holder
  computing, a second process at its fd limit: the callback runs twice, the waiter does not get the holder's value).
- **No readable `/proc`** (a chroot'ed fpm pool without `/proc`, a seccomp or LSM profile that denies it). *By reading*:
  the same `return 0`. With `/proc` missing for everyone the ids agree (start time 0 on both sides) and a recycled pid
  looks alive instead: a dead holder is then never detected.

**Failure mode as the user sees it.** Random segfaults in php-fpm workers and CLI commands, occasionally a value that is
not the one stored under that key, `lock_steals` climbing while nothing died. `size-and-monitor.md:69` reads a non-zero
`lock_steals` as "a worker died"; here it means the arena is being corrupted.

**Docs.** [Sharing](sharing.md) says "Separate containers do not share" and the how-to's troubleshooting table says
"different containers: they cannot share". Both describe an intention as a fact: nothing stops them, and the common way of
"sharing the cache with a sidecar" is exactly the broken case. The fd case is not mentioned anywhere.

**Mitigation, cheapest first.**
1. *Fail closed* (a few lines in `proc_start`): only `ENOENT`/`ESRCH` mean gone; any other error means "cannot tell",
   treated as alive. Cost: none on any fast path; a holder that really died while the checker is out of fds is stolen on a
   later check. Turns test 095 green.
2. *Refuse another PID namespace at attach*: the creator stores the inode of `/proc/self/ns/pid` in the region header, an
   attacher with another one gets "no cache" and the usual warning. Cost: one `stat` per process start. This makes the
   documented rule true.
3. *Docs*: say "must not share `/dev/shm` across PID namespaces", not "cannot".
4. Redesign (robust futexes, or a kernel-held lock per shard) stays rejected on the measured cost; 1 + 2 remove the
   corruption without it.

### 2. The pin cap is per request, not per arena

**Premise attacked.** "A request keeps at most a quarter of the segments; past that its fetches copy" bounds the damage
of long requests, and "long-running processes use copy mode" covers the rest.

**Evidence.** The cap counts one worker's kept segments (`arena/pool_read.h:52-60`); nothing sums over workers. Copy mode
is enforced by SAPI name only (`nocopy.c:390-399`): php-fpm, mod_php, LiteSpeed, `php -S` and classic FrankenPHP are
"short-lived" whatever a request does. A merge that picks a pinned victim still evicts its entries; the memory comes back
only when the pin goes ([Architecture](architecture.md#merge-segcache-style-compaction-and-eviction-in-one)), and the
allocation loop runs up to `2 * fair_segs` merges under the shard lock looking for room (`arena/shard.c:297`).

Reproduced: default 32 MB (237 segments), 30,000 keys that fit (baseline: every read hits), 4 CLI workers doing random
put/get. Meanwhile N zero-copy requests (`php -S`, `PHP_CLI_SERVER_WORKERS`) each read the keys once, keep what they
read and then sleep, as a request waiting on a slow database does:

| long requests | pinned segments | entries left | hit ratio of the other workers |
|---|---|---|---|
| 0 | 0 | 30,000 | 100.0% |
| 1 | 70 | 24,612 | 81.8% |
| 2 | 107 | 19,102 | 63.3% |
| 4 | 179 | 8,277 | 26.9% |
| 8 | 194 | 5,981 | 19.3% |

The design-decisions entry measured one request (88.7% with the cap); the missing case is several at once.

**Failure mode.** The moment the backend slows down (a locked table, a slow upstream API), requests run long, their pins
stay, the cache empties, more requests miss and hit the slow backend. The cache fails when it is needed most, and recovers
only as those requests end. Also any deliberately long fpm request: exports, imports, server-sent events.

**Docs.** The cap and "use copy mode for long-running processes" are documented; that php-fpm requests count as short
however long they run, and that the cap multiplies by the number of such requests, is not.

**Mitigation.**
1. *Doc + stat*: say it; expose how many requests reached the cap (the Drupal run in design-decisions counted 0.7%); alert
   on `pinned_segments` near `segments`. Recommend `request_terminate_timeout`.
2. *A global cap*: a shared count of kept segments (one atomic add per newly kept segment, not per fetch); a request
   switches to copy mode when the arena-wide count passes a quarter. Cost to be measured with `./dev ab main 7 read`;
   expected at the noise floor since `nocopy_keep` already runs once per new segment.
3. *Age*: a request older than a few seconds copies from then on. It cannot release what it already holds, so it bounds
   growth, not the tail.

### 3. "No cache" is silent exactly where it matters

**Premise attacked.** "A cache that is off is slow; a cache that is split is wrong", so a process that cannot attach gets
no cache and that is the safe default.

**Evidence.** The rule protects readers. It does not protect *invalidation*, which is what the CLI is mostly used for:

- `nocopy_clear()` returns `true` with no arena (`nocopy.c:128-133`, `na_clear` in `nocopy_store.c:400`). Reproduced:
  without `shm_name`, `nocopy_put` false, `nocopy_get` NULL, `nocopy_delete` false, `nocopy_clear()` **true**.
  A deploy script that clears the cache from a CLI that did not attach reports success while php-fpm keeps serving what it
  had. That is the split-cache outcome the rule exists to prevent.
- The likeliest way not to attach is the ordinary one: the CLI runs as root or the deploy user, php-fpm as `www-data`
  (`arena/region.c:24-28`). The project hit it itself: `./dev drupal-bench` "ran without a cache" until someone noticed
  ([Status](../status.md), roadmap item 4).
- The reverse order is worse. Reproduced: root creates `/dev/shm/squat.<id>` first (a cron job at boot); every `www-data`
  process then gets "no permission to open it" and has no cache; `www-data` cannot remove the object (`/dev/shm` is
  sticky) and nothing else does. The whole pool runs at a 100% miss rate until someone removes the file by hand. Any local
  user can do this on purpose: the name is predictable from the world-readable `nocopy.so`.
- Visibility: one startup warning per process. In CLI it reaches stderr (also with `display_startup_errors=0`); under a
  root fpm master it is one line per worker in the fpm log (`nocopy.c:516-520`). After that every call fails softly.
  `nocopy_stats()['shm_name']` is `null` and `nocopy_shared_with()` is `none`, for whoever asks. The Drupal module asks
  and throws; the APCu polyfill and the Symfony adapter do not.

**Failure mode.** Stale pages after a deploy although "the cache was cleared"; or a site that is simply slower than it
should be for weeks, with a correct-looking configuration.

**Docs.** The rule and its warnings are documented well ([Sharing](sharing.md), the how-to's troubleshooting table). Not
documented: that `nocopy_clear()` succeeds without a cache, that a foreign-owned object must be removed by hand, and that
this is the common failure, not an exotic one.

**Mitigation.**
1. `nocopy_clear()` returns `false` without an arena (a behaviour change; one line). `nocopy_delete` already does.
2. A documented health check for deploy scripts: `php -r 'exit(nocopy_shared_with() === "none");'`, run as the user the
   commands run as; the CLI console (`cli/`) exiting non-zero on `none`.
3. The warning for a foreign-owned object names the owner and the `rm` to run.
4. Not recommended: a private fallback cache. The premise is right about that.

### 4. The layout id is read from disk when the arena is opened

**Premise attacked.** "The layout id hashes the loaded `nocopy.so`, so processes share only when they run the same code."

**Evidence.** `layout_id()` opens the path `dladdr` reports and hashes the file that is there *now* (`nocopy.c:362-381`).
It runs inside `arena_open()` (`nocopy.c:499`), which under a root fpm master is each worker's first request
(`nocopy.c:579-583`), possibly long after the code was loaded.

Reproduced: php-fpm with a root master and two static workers. Worker 1 serves a request (`/lid.3d10eb94`). `nocopy.so`
is replaced on disk, as a package upgrade does. Worker 2, forked from the same master with the same code in memory, serves
its first request and opens `/lid.c6734d2a`: a put through worker 1 is `NULL` in worker 2. One pool, two caches, until
php-fpm is restarted; every respawned worker (`pm.max_requests`, `pm = dynamic`) joins the new one. Old code now also
shares an arena with any process that really loaded the new file, which is the case the id exists to exclude.

Related, same mechanism: other builds' arenas are removed only by `collect()`, which runs only at open
(`arena/region.c:255`). Reproduced: an old build's process exits while a new build's process keeps running: both objects
stay (2x `shm_size`, reserved by `posix_fallocate`), until some later process opens. With a non-root fpm master the open
is once, in the master: the old arena stays until the next start.

**Failure mode.** Between upgrading the package and restarting php-fpm: a split cache inside one pool (stale reads after
deletes), then possibly mixed layouts. After an upgrade done by reload: twice the memory in `/dev/shm`.

**Docs.** The how-to says "restart php-fpm after replacing it". The warning "another build is in use" is logged at notice
level per worker. That the id is not the loaded code's is not documented, because it is not intended.

**Mitigation.** Compute the id once in MINIT, when the file on disk is the file just loaded, and keep it (the hash cost is
already paid there: +0.24-0.5 ms, measured in design-decisions). The remaining window is microseconds, and a build id
compiled into the binary would close it. For leftovers: run `collect()` also at a worker's request end every N minutes;
one `readdir` of `/dev/shm`, to be measured.

**What is not a problem here** (suspect 5 as stated): a deploy does not start a cold arena unless `nocopy.so` changed. A
PHP patch release with the same `nocopy.so` keeps the arena; a new container has a new `/dev/shm` anyway.

### 5. Valkey: a process that starts no new request never re-checks

**Premise attacked.** "A request stays `synced` for its whole length, so a listener that dies mid-request serves it stale
data until it ends: by design" ([audit](audit-2026-11.md#ruled-out)). That bounds staleness only if requests end.

**Evidence.** `vk_ready()` returns at once when `synced` is set (`nocopy_valkey.c:721`); `synced` is cleared in RINIT,
per FrankenPHP worker request, and when this worker's own command connection fails. A CLI process is one request: a queue
consumer, a daemon, a Swoole or RoadRunner worker. Local hits send no command, so nothing clears it.

Reproduced: a CLI script stores and fetches a key; the listener is killed; the key is changed in Valkey from outside. For
the next 5 s, 100 of 100 fetches in that process returned the old value, with `nocopy_stats()['valkey_listener']` at 0. A
new process saw the new value at once (it restarted the listener). On a host that runs only long-lived workers nobody
restarts it.

**What a user can rely on, in one sentence.** A read returns a value Valkey held at some moment after roughly one second
before this process's current request made its first cache call (so: arbitrarily old in a process that never starts a new
request), writes are last-arrival-wins at Valkey and visible on other servers only after their listener applied the push,
and only `add`, `inc` and `cas` are decided atomically, in Valkey.

**Partition and failover** (*by reading*). Valkey unreachable: fetches miss and writes fail, by design; each worker spends
up to `valkey_timeout` (0.5 s) per attempt with 0.5-1.5 s between attempts. A blackholed server therefore costs each
worker a quarter to half of its time on top of the misses; to be measured. A failed-over primary behind one address: the
listener reconnects and clears, which is safe; writes acknowledged by the old primary and lost in replication are lost for
everyone, as with any Redis cache. A local write that reached the arena and then timed out at `SET` is visible to this
host's other workers until it is dropped (`nocopy_valkey.c:1033-1048`): up to the timeout, new-then-old. Sentinel and
Cluster are on the roadmap, not supported.

**Docs.** [Sharing](sharing.md) says "Never serve what might be stale"; the audit records the per-request exception. The
long-running case is the contradiction.

**Mitigation.** Let `synced` expire: re-check the heartbeat when the last check is older than, say, 250 ms. One coarse
monotonic clock read per fetch (the realtime coarse clock cost 3.5 ns in design-decisions; this one to be measured with
`./dev ab`), or a check every N calls with no clock at all.

### 6. Valkey TTLs use the writer's wall clock

**Premise attacked.** TTLs are whole seconds of the wall clock, and that clock is one clock.

**Evidence.** A write sends `SET … PXAT (now + ttl + 1) * 1000` computed from the PHP host's clock
(`nocopy_valkey.c:933`, `:1044`), and the payload carries the same absolute second, which every reader compares to its
own clock (`nocopy_valkey.c:982`). Reproduced on the Valkey side: `SET k v PXAT <5 s ago>` answers `OK` and `GET` returns
nil. So a writer whose clock is behind by more than a TTL stores nothing and is told it did; one that is ahead stores for
TTL + skew; a reader that is ahead treats live entries as expired and recomputes them. `inc` already uses a relative `PX`
(`nocopy_valkey.c:859`). Design-decisions rejected Redis's `last_delete_all` for "comparing clocks of different servers";
this does the same.

**Locally** the wall clock is one clock, and a step is survivable: forward expires entries early (a burst of misses) and
frees leases early (a duplicate compute); backward keeps entries for the step longer than their TTL. `seq` stays
monotonic (`arena/shard.c:332`). Code that uses a TTL as a correctness bound (a lock, a rate limit) inherits the step.
*By reading*; no reproduction.

**Docs.** Not mentioned.

**Mitigation.** `PX ttl_ms` on `SET` (no cost), keeping the absolute second in the payload for local copies; document
that servers need synchronized clocks for TTLs shorter than their skew.

### 7. A named file full of pointers, at a fixed address

**Premise attacked.** Mode `0600` and the owner check make the arena as private as APCu's or OPcache's memory.

**Evidence.** Those are anonymous mappings. The arena is a file (`arena/region.c:235`) that any code running as the pool's
user can open with `fopen()`: no extension, no memory-safety bug. Readers follow its pointers into userland.
Reproduced: a plain PHP script (`php -n`, nocopy not loaded) finds a cached value in `/dev/shm/trust.<id>` and rewrites
six pointers with `fwrite`; the next process that fetches the key dies with SIGSEGV. A crash is the mild case: a forged
`zval` is a read primitive, a forged object or array a write or call primitive, in every process that fetches the key,
including other pools of the same user with other `shm_name`s (their files are equally writable).

So "write a file as `www-data`" becomes "run native code in every worker", past `disable_functions`, and on read-only
deployments where `/dev/shm` is one of the few writable places. `open_basedir` blocks it only if `/dev/shm` is outside.

The mapping address is fixed (`arena/region.c:80`): the first pointer found was `0x200000890808` and will be in every
process on every host. Attacker-influenced cache content at a known address weakens ASLR for any other memory-corruption
bug in PHP. OPcache and APCu memory moves with each master start.

**Docs.** [Sharing](sharing.md) says "anyone who can write it controls what php-fpm reads (it hands out pointers into
it)". True, and it reads as a statement about other users. It does not say that every script of the same user is such a
writer, nor anything about the address.

**Mitigation.** Docs first: a "Security" section saying what the trust boundary is (one uid = one trust domain; do not
share a uid between sites; keep `/dev/shm` out of `open_basedir`). A random base chosen by the creator and stored in the
header (attachers already map at `hdr.base`, `arena/region.c:178`; `ar_base` is already a variable) costs nothing at run
time and restores per-boot ASLR. Validating pointers on read would cost every fetch and is not proposed.

### 8. A stopped lock holder is waited for forever

**Premise attacked.** A holder is either running (and done in microseconds) or dead (and stolen from).

**Evidence.** `ar_lock_wait` spins and yields without bound while the holder is alive (`arena/proc.c:80-100`).
Reproduced: 4 writers churning a 16 MB arena are sent SIGSTOP; a fresh process then tries 4,000 puts. In 4 of 6 rounds it
was still spinning after 5 s (killed by `timeout`); in the other 2 no stopped writer held a lock and it took 16 ms.
`docker pause` of a sidecar, the cgroup freezer, a debugger, a checkpoint do the same. `max_execution_time` does not
fire inside the spin; only `request_terminate_timeout` ends it.

The bounded variant is CPU throttling, measured for suspect 2:

- One cgroup, 8 workers, `--cpus=0.5` (`bench/tail.php`, `evict`): put p99.9 1.2 -> 90.9 ms, get max 8.8 -> 90.5 ms;
  APCu put p99.9 1.4 -> 79.8 ms. `contend`: put p99.9 4.2 -> 94.9 ms, APCu 8.4 -> 103.5 ms. The tail *is* the throttle
  (lock-free gets stall the same way); the spinlock adds nothing APCu's lock does not.
- Two cgroups on one arena (shared PID and IPC namespaces; 4 writers at `--cpus=0.2`, 2 measured workers unthrottled):
  the unthrottled workers' put p99.99 0.39 -> 0.92 ms and max 4.2 -> 102.9 ms; gets unchanged; 0 steals. A throttled
  neighbour holding a shard lock stalls an unthrottled writer for up to one throttle period.

**Docs.** Not mentioned. Status lists the 1-1.8 ms p99.99 as open; under a quota it is the quota's period.

**Mitigation.** Docs (a writer can wait as long as the slowest process sharing the arena is descheduled; do not freeze
processes that share one). A spin budget after which a put returns `false` is the redesign: cheap in code, but a failed
write is new behaviour for callers; to be decided, not measured.

### 9. 1,024 worker slots, then every call throws

**Evidence.** `NOCOPY_MAX_WORKERS` is 1,024 (`arena/base.h:24`), per arena, counting processes and ZTS threads. A process
that gets no slot is attached but untracked, and every `nocopy_*` call throws `Error: nocopy: no free worker slot`
(`nocopy_store.c:14-21`, `nocopy.c:584-586`). *By reading*; reaching it needs 1,025 live processes.

**Failure mode.** Past the limit the cache does not degrade to misses like every other failure here: requests die with
an uncaught `Error`. Large hosts (several pools on one name, FrankenPHP threads plus CLI workers) are where it bites, at
peak.

**Docs.** Only the benchmarks page mentions the number.

**Mitigation.** Treat "no slot" like "no arena" (misses and failed writes, one log line): consistent with finding 3's
visibility fixes. Document the limit in the configuration reference.

### 10. Fixed-at-creation settings are ignored silently on attach

**Evidence.** An attacher takes the creator's size and tables (`arena/nocopy_arena.h:21`, `arena/region.c:171-181`
compare only the build id). Reproduced: with a 16 MB arena mapped by one process, a second process configured with
`shm_size=64M` and `entries_hint=1000000` reports 16 MB and 65,536 slots, with no warning. The arena is recreated only
when no process maps it; with cron, queue workers or a graceful reload overlapping, that can be never.

**Past each limit** (suspect 8), all *by reading* unless noted: slot table full: eviction by count before memory is full
(documented, `entries_hint`); an entry larger than a shard's share minus one segment: `nocopy_put` returns `false`
(`arena/shard.c:275`; 7.4 MB at the default 32 MB); atoms' halves full: strings stored per entry again (documented);
shard count: fixed, no failure mode; workers: finding 9; size: this finding.

**Mitigation.** Warn at attach when the creator's size or slots differ from this process's settings (one comparison at
start). Document "stop every process, CLI included, to resize".

### 11. Engine internals without an upper version guard

**Evidence.** The value layer writes PHP's own structures into shared memory: about 80 lines touch `HashTable` layout and
flags, refcount and type-info words, object property tables and ext/date internals (`grep` over `nocopy_*.c|h`:
13 hash-layout, 13 refcount, 9 type-info, 17 object-layout, 18 ext/date, 5 property-info lines). The only version check is
a lower bound (`nocopy.c:8-10`). The layout id contains `ZEND_EXTENSION_BUILD_ID`, so arenas never cross PHP minors, but
the *assumptions* do: a PHP 8.6 that writes a field of an immutable array, treats interned-but-not-permanent strings
differently, or reorders a `zend_object` builds without a warning.

What would break silently rather than crash: an engine write into an arena array (cross-process data race, wrong values);
a changed meaning of the refcount-3 marker for deduped strings; `zend_hash_find_known_hash` callers multiplying (bug 2 of
the audit was this class).

**Is there a test that would catch it?** Partly. Test 093 compares the arena byte for byte after userland operations and
`./dev debug` runs the engine's assertions: both catch engine writes for the operations they list. Nothing catches a
layout change that still round-trips within one build, and CI runs 8.4 and 8.5 only.

**Mitigation.** `#error` (or a `config.m4` failure with an override) for a PHP minor above the last one CI runs, so a new
minor is a decision, not an accident. Add PHP's development branch to CI as a non-blocking job running 093 and `debug`.

## Ruled out

So nobody repeats it.

- **Fixed-address mapping fails badly across unrelated processes** (suspect 3). *By reading.* The bases
  (`0x2000'0000'0000`, else `0x10'0000'0000`, `arena/region.c:80`) are far from where x86-64 and arm64 kernels place PIE
  binaries, heaps and mmap regions under ASLR, also with 5-level paging (user space stays below 47 bits unless asked);
  the second fits 39- and 42-bit address spaces; ASan has its own entry. An attacher whose address is taken gets "its
  address is taken in this process" and no cache (`arena/region.c:178-181`): never a second arena, since the creator's
  base is in the header. So the failure mode is finding 3's, not a split. The address being *predictable* is finding 7.
- **Spinlocks under a CPU quota make the tail unbounded** (suspect 2). Measured above: inside one cgroup the tail equals
  the throttle period and matches APCu's; across cgroups it is bounded by the neighbour's period. Unbounded only for a
  stopped holder (finding 8).
- **Every deploy starts a cold arena and doubles memory** (suspect 5). Only when `nocopy.so` changes; see finding 4 for
  what does go wrong then.
- **Wall-clock expiry on one host** (suspect 9). Expired entries are never served (`arena/entry.h:40`); they stay
  allocated until a write in their shard, which costs nothing since the memory is reserved up front
  ([Status](../status.md) already says so). Stats overcount meanwhile. Clock steps: finding 6, last paragraph.
- **"Use copy mode" is only advice** (suspect 6). It is enforced, by SAPI name; Swoole and RoadRunner run under `cli` and
  copy. What is not covered is a long request under a short-lived SAPI: finding 2.
- **Entry size, slot tables, atoms, shard count past their limits** (suspect 8): each ends in a `false` or an eviction
  and is documented, except the two in findings 9 and 10.
- **Unseeded key hash, robust mutexes' speed, `request_scope`, one listener per arena**: measured in
  [Design decisions](design-decisions.md); not reopened. The single listener is sound for requests that end; finding 5
  is about the ones that do not.
- **The all-or-nothing rule itself** (suspect 11): a private fallback would be worse. The flaw is its visibility and
  `nocopy_clear()`'s answer, finding 3.

## Not done

- Findings 1 (no `/proc`), 5 (partition timing), 6 (local clock steps), 9 and 11 are by reading, as marked.
- Nothing ran on arm64, under FrankenPHP worker mode, or with ZTS.
- No mitigation was implemented or measured; costs marked "to be measured" are estimates.
