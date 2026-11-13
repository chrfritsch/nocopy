# Refactoring plan (2026-11-13)

A survey of the C code and the integrations against the classic software-craft books, for contributors. No behaviour, API,
INI or stats change; nothing here reintroduces an idea [design-decisions](design-decisions.md) rejected with numbers.
Baseline: `./dev test` 107 passed, 2 skipped (8.5, cloud sandbox, amd64).

The code has had several such passes already (the arena split into modules and its named-steps refactor, the value
layer by phase, `./dev layers`), so what is left is mostly small: knowledge of one format spread over two or three
places, a few magic numbers, a macro that hides a `return`, names that look like API, two long functions, and stale
comments. The arena, refactored in 384f618, has little left.

## How each step is checked

- Per commit: `./dev test`, `./dev layers`; arena locking, eviction, pins or shards: `./dev stress 30`, `./dev c 5`;
  `nocopy_resp.c`: `./dev noresp`, `./dev shm`; `apcu/`: `./dev apcu`.
- Hot paths (build, write, read, store, kept copies, arena): first compare the release `nocopy.so`'s disassembly
  before and after (addresses stripped). Identical code needs no A/B, and the commit says so. Otherwise
  `./dev ab HEAD 7 <groups>` and the numbers go in the commit message; a measurable loss is reverted and recorded in
  design-decisions' rejected table. The unity TU moves rows ±3-6% on layout alone (design-decisions, Build), so
  "measurable" means outside HEAD-against-itself noise in two runs.
- Before finishing: `./dev ci`.

## Findings, by value / risk

Principles: **O** Ousterhout (deep modules, information hiding, errors out of existence, comments), **F** Fowler /
Feathers, **P** Pragmatic Programmer (DRY, orthogonality, broken windows), **C** Code Complete / Practice of Programming
(naming, conventions, cohesion, idiom).

| # | Finding | Principle | Where | Refactoring | Risk | Hot |
|---|---|---|---|---|---|---|
| 1 | How an arena string's header is written (refcount 2, `IS_STR_INTERNED`, never `IS_STR_PERMANENT`, UTF-8 flag kept) is spelled out three times; the invariant's comment sits on one of them | O information hiding, P DRY | `nocopy_build.c:105-106` (`atom_sight`), `:117-119` (`build_copy`), `nocopy_value_int.h:384-385` (`nc_write_flat`) | Extract Function: `arena_str_type(s)` in `nocopy_value_int.h`, the invariant's comment there | low | write (inline, expect identical code) |
| 2 | Where a block's value area starts (`KEY_OFF + SALIGN(klen)`) is computed in four places across write and read | O information hiding, P DRY | `nocopy_value.c:318`, `:327`, `:350`; `nocopy_read.c:461` | Extract Function: `val_off(klen)` beside `KEY_OFF` | low | write (identical code expected) |
| 3 | `date_word`'s bit layout is encoded in the header and decoded with bare shifts and masks in another unit | O information hiding, C magic numbers | `nocopy_value_int.h:191-195`; `nocopy_class.c:198-217` | Move the decoders next to the encoder (`date_word_us/zone_type/dst/offset`), named field widths | low | no |
| 4 | "Smallest power of two, from 4 / `HT_MIN_SIZE`, at least n" is written as a loop three times | P DRY, C | `nocopy_value_int.h:47-54` (`map_mask`), `:61-68` (`sparse_mask`), `nocopy_build.c:807-810` | Extract Function: `pow2_at_least(min, n)` (the same loop: `__builtin_clz` was rejected, design-decisions Writes) | low | write (identical code expected) |
| 5 | An array's bytes in a block, `SALIGN(sizeof(zend_array)) + SALIGN(data_size(a))`, twice | P DRY | `nocopy_build.c:208`, `:236` | Extract Function: `array_bytes(a)` next to `data_size` | low | write |
| 6 | The kept-copy set index hard-codes 1024 sets as `>> 54`, and the victim rank's "none yet" as `4`, beside `#define NOCOPY_KEPT 4096` | C magic numbers, P DRY | `nocopy_kept.h:9-10`, `:43-44` | Replace Magic Number: `NOCOPY_KEPT_SET_BITS 10`, `NOCOPY_KEPT = WAYS << SET_BITS`, a named rank | low | copy-mode read (identical code expected) |
| 7 | `OUT_OF_REGION` is a macro that evaluates a call and `return`s from its caller: control flow hidden in a macro | C / Practice of Programming (macros), O obvious code | `nocopy_build.c:1071-1078`, used at `:1105`, `:1108`, `:1112` | Replace Macro with Function: `region_leave(b)` / `region_back(b, saved)` around the call, the `return` written out | low | write (objects, references only; `noinline` callees) |
| 8 | The RESP payload header's layout (flag at 1, expiry at 2, compute ms at 10, value at 14) is magic offsets repeated in encode and decode | O information hiding, C magic numbers | `nocopy_resp.c:987-1036` | Replace Magic Number: named offsets derived from each other, `_Static_assert` on `VK_HDR`. Wire format byte-identical (tests 05x, 101, 105) | low | no |
| 9 | The tier's detour functions are `static` but named like public API (`nocopy_vk_fetch`, …, 9 of them); the no-arena table's are `na_*` | C naming conventions | `nocopy_resp.c:1069-1258` | Rename Function: `tier_fetch`, `tier_store`, … (`./dev layers` greps `nocopy_vk*` names, so the six real calls stand out) | low | no |
| 10 | Stale or misleading comments: `nocopy_value_int.h:1` "plan -> write" (planning went 2026-11-12); `nocopy_arena.h:39` says `nocopy_set_clock` replaces the *wall* clock (it is the monotonic one since 2026-11-10); `nocopy_arena.h:120` and `shard.h:65` name `nocopy_put_abort` (see 16) | P broken windows, O comments | as listed | Fix the comments. `nocopy_arena.h` changes in comments only: **needs your OK** | low | no |
| 11 | The thread buffer's keep limits are bare numbers (1 MB, 256 K) though architecture.md names the rule | C magic numbers | `nocopy_value.c:285`, `:290` | Replace Magic Number: `ENV_BUF_KEEP`, `ENV_REL_KEEP` | low | no (request end) |
| 12 | `nocopy_remember` (57 lines) mixes the XFetch decision, waiting for a lease holder and computing | F long function, C cohesion | `nocopy_store.c:314-371` | Extract Function: `xfetch_due(exp, delta, beta)` (the formula, pure) and `await_holder(...)` (the poll loop) | low-med | `remember` hit (A/B `apcu_entry hit`) |
| 13 | `build_array` (113 lines) is three phases: the sharing decision, copying the table (header, hash, rechaining, sparse as map), walking the elements | F long function / Split Phase, O | `nocopy_build.c:766-879` | Extract Function: `copy_table(b, a, &layout)` for the middle phase, `zend_always_inline`; the walk stays | med | **write, hottest** (A/B `write`; revert on loss) |
| 14 | `build_node` (98 lines): first switch decides what the node keeps (runs hooks), second writes it | F Split Phase | `nocopy_build.c:882-980` | Extract Function: `node_prepare(b, n, o, kind)` for the first switch; one switch per phase kept (`-Wswitch`, architecture.md) | med | write objects (A/B `store object`) |
| 15 | `graph_build`'s failure branch (objects never constructed: no destructor, slots unset) is inline in a 55-line function | F Extract Function, O | `nocopy_read.c:382-394` | Extract Function: `objects_abandon(g, objs, made)` | low | read graphs, cold branch |
| 16 | Dead API: `nocopy_put_abort` has had no caller since every block is placed under the lock (2026-11-12); `nocopy_stats_t.start_time` is filled but nothing reads it | P broken windows, O | `arena/shard.h:65,76`, `arena/shard.c:418-422`, `scripts/check-layers.sh` (data regex); `arena/nocopy_arena.h:136`, `arena/arena.c:148` | Remove `nocopy_put_abort`. Leave `start_time` (architecture.md names it as wall time leaving the arena). **Changes the arena's API: needs your OK** | low | no |
| 17 | `nocopy.c` holds a self-contained concern (the layout id: ELF build-id note, Mach-O UUID, file hash; 85 lines) among the PHP functions, INI and module | C cohesion, O deep module | `nocopy.c:367-450` | Move Function into `nocopy_layout_id.c` (one call, `layout_id()`; included in the unity TU like the others) | low-med (build plumbing: `config.m4`, `pie`, `ab.sh`) | no |

Order of execution: 10 (comments, if approved), 1, 2, 5, 4, 6, 3, 11, 7, 8, 9, 15, 12, 14, 13, 16 (if approved),
17 (optional). Every item is one commit, named after its refactoring.

Characterization first where coverage is thin: items 1-6 and 13-14 are covered by `block_check` (asan, debug, fuzz)
and the value tests; 3 by the date tests (dates across zone types, test 109's neighbours) and I will add a `.phpt` that
round-trips each zone type, offset and DST flag explicitly if none pins all of them; 8 by tests 05x/101/105 (a payload
written before must decode after: I add a test that decodes a fixed byte string, so a changed offset fails); 12 by
the `remember` tests (single flight, XFetch, stale-while-revalidate).

## Considered and left alone

- **Copy mode's keep rule and read dispatch duplicated** (`nocopy_kept_read` repeats `nocopy_value_read`'s plain/scalar
  dispatch; `nocopy_kept_get` / `_get_graph` repeat the budget check; the `zend_memory_usage` bracket twice in
  `nocopy_kept.h`): one shared version was measured and rejected (design-decisions, last row of the table: -2 to -6%).
- **`nocopy_value_scalar` before `nocopy_value_read`**: the same test twice on purpose (Build: -5% for zero-copy scalars without it).
- **One array walker for all walks** (build, rebase, check, copy-out, templates): rejected, -2.7% on array stores.
- **Two `switch`es on the class kind per phase**: deliberate, so `-Wswitch` names a phase that misses a kind.
- **Splitting `nocopy_resp.c` (1,270 lines)**: one module behind a six-call header and a detour table: a deep module;
  splitting would turn its statics into a second internal header. Its sections are already ordered by concern.
- **The RESP fill guard (`vk_gen()` before, compare after) in three ops**: two lines each and the order is the point;
  a helper would hide which read it brackets.
- **The grow-in-place pattern of the builder's inline arrays** (`str_grow`, `hv_grow`, `graph_b_node`, …): each is
  `noinline` on purpose, `inl_grow` already holds the shared part.
- **The key match `h == h && klen == klen && nocopy_key_eq`** in `nocopy_find_ex` and `locate`: one inline would
  touch the public header's read path, which is forced inline and measured (+9 ns when outlined); not worth it for one line.
- **The single translation unit, the arena's process-local globals, the inline read path**: measured (Build).
- **`apcu_put()` in the polyfill**: a helper with a global `apcu_*` name, but callable, so part of the package's surface.
- **`ponytail:` comments**: the project's tag for known, documented limits, not stale TODOs.
- **The arena modules**: refactored in 384f618 (named steps, no flag arguments, one place per constant); nothing found
  that pays for the risk under the shard locks.
