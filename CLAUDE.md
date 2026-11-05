# nocopy

PHP extension (C): zero-copy shared-memory cache, APCu-compatible via a PHP polyfill. Read before changing things:
`docs/` (Diátaxis: tutorials, how-to, reference, explanation; index `docs/README.md`). Before C changes: `docs/explanation/architecture.md` (invariants)
and `docs/explanation/design-decisions.md` (what was measured and rejected). State and roadmap: `docs/status.md`.

## Build and test

No PHP on the host: everything runs in Docker through `./dev` (CI runs the same commands). `./dev help` lists all.

- `./dev test` after any C change; `./dev test tests/0xx.phpt` for one test; `PHP=8.4 ./dev test` for another version
- Locking, eviction, pins, shards: also `./dev stress 30` and `./dev c 5`
- Valkey code (`nocopy_valkey.c`): also `./dev novalkey` (core must build without it) and `./dev shm`
- Polyfill (`apcu/`): `./dev apcu` (23 known failures, listed in `scripts/apcu-suite.sh`); Symfony bundle: `./dev symfony`; Drupal module: `./dev drupal`
- Before calling something done: `./dev ci`
- Poke at it by hand: `./dev shell`

## Layout

- `nocopy.c` PHP API + local store; `nocopy_arena*.c|h`, `nocopy_evict.c`, `nocopy_lease.c`, `nocopy_owner.c` the shared-memory arena (plain C, tested alone by `tests/c/`)
- `nocopy_valkey.c` Valkey tier, reached only via `nocopy_vk_*` (connections: vendored libvalkey in `third_party/`, updated only with `scripts/vendor-libvalkey.sh <tag>`); compiled out with `--disable-nocopy-valkey`
- `nocopy.stub.php` -> `nocopy_arginfo.h` (`./dev stub` after API changes)
- `tests/*.phpt` suite (05x need Valkey, skip without); `docker/` images + in-container scripts; `bench/` benchmarks
- `apcu/`, `symfony/`, `drupal/`: integrations, each its own composer package

## Rules

- Commit straight to `main`.
- Code comments: one short line, or none.
- Performance claims need numbers (`./dev bench`, A/B via `docker/ab.sh`); record decisions and rejections with numbers in `docs/explanation/design-decisions.md`.
- Tests give every arena its own `nocopy.shm_name` and remove `/dev/shm/<name>.*` afterwards.
- Docs: keep them in the Diátaxis quadrant they belong to; update the reference pages (functions, configuration, stats, dev-commands) with any API, INI or `./dev` change, and `docs/status.md` after verifying runs or finishing roadmap items.
