# igbinary Serializer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Values that nocopy serializes (objects, shared references) can be written with igbinary instead of `serialize()`, selected by `nocopy.serializer` / `apc.serializer`.

**Architecture:** igbinary's C functions are resolved at MINIT via `dlsym`, so there is no build-time dependency. `serialize_value()` and
`cache_unserialize()` in `nocopy.c` dispatch on the configured serializer and on a per-entry flag. `apc.serializer` becomes an alias like `apc.shm_size`.

**Tech Stack:** C (PHP extension API, PHP 8.3+), igbinary 3.x (PECL), phpt tests, Docker (`nocopy-linux` image).

**Spec:** `docs/superpowers/specs/2026-11-04-drupal-integration-design.md` (Phase 1)

## Global Constraints

- INI `nocopy.serializer` = `php` (default) | `igbinary`, `PHP_INI_SYSTEM`. Alias `apc.serializer`, with the same rules as the other aliases (`nocopy.*` wins, with a startup warning). APCu's value `default` means `php`.
- `igbinary` configured but the igbinary extension not loaded: E_CORE_WARNING at startup, then behave as `php`.
- Each serialized entry records which serializer wrote it. Reads decode by the flag, never by the INI.
- No build-time dependency on igbinary. `config.m4` is unchanged.
- PHP 8.3+, Linux. Existing tests must keep passing (`docker/test.sh`).
- Comments: one short line, or none if the code is self-explanatory.

## Review Focus

- An entry flagged igbinary read while igbinary is unavailable (cannot happen with one INI per arena, but must not crash): the read returns a miss. Covered by a code path, not a test.
- An unknown value such as `nocopy.serializer=msgpack`: startup warning and `php`, never a failed startup. Test in Task 1.
- `apcu_inc`/`apcu_cas` on a serialized entry keep refusing it: `ENTRY_SERIALIZED` stays set alongside the new flag. Test in Task 1.
- Objects with `__serialize`/`__unserialize` and shared references round-trip identically under igbinary. Test in Task 1.
- `ini_get('apc.serializer')` shows the value in effect (the `sync_aliases` behavior). Test in Task 1.

---

### Task 1: `nocopy.serializer` with igbinary

**Files:**
- Modify: `nocopy.c` (`#define ENTRY_SERIALIZED` at :39, `serialize_value` :434, `build` :452–495, `cache_unserialize` :570, INI tables :991–1048, `sync_aliases` :1052, `PHP_MINIT_FUNCTION` :1089, module entry :1194)
- Modify: `docker/Dockerfile` (install igbinary without enabling it, like apcu)
- Modify: `README.md` (configuration table; "Differences from APCu" row for `apc.serializer` at :164)
- Create: `tests/016_igbinary.phpt`, `tests/017_serializer_fallback.phpt`, `tests/018_apc_serializer_alias.phpt`

**Interfaces:**
- Produces: INI `nocopy.serializer`; entry flag `#define ENTRY_IGBINARY 2`, always set together with `ENTRY_SERIALIZED`.

- [ ] **Step 1: Add igbinary to the Docker image**

In `docker/Dockerfile`, after the apcu line: `&& pecl install igbinary && rm -f /usr/local/etc/php/conf.d/*igbinary*`. Rebuild: `docker build --build-arg PHP=8.5 -t nocopy-linux:8.5 docker`.
Check: `docker run --rm nocopy-linux:8.5 ls /usr/local/include/php/ext/igbinary/igbinary.h` exists. Read it to confirm the exported signatures:
`int igbinary_serialize(uint8_t **ret, size_t *ret_len, zval *z)` (result emalloc'd) and `int igbinary_unserialize(const uint8_t *buf, size_t buf_len, zval *z)` (0 = success).

- [ ] **Step 2: Write the failing tests**

`tests/016_igbinary.phpt`. SKIPIF: igbinary.so is missing from `ini_get('extension_dir')`. INI: `extension=igbinary`, `nocopy.serializer=igbinary`, `apc.enable_cli=1`.
```php
class P { public $a = [1, 2]; public ?P $self = null; }
class S { public $x = 1; function __serialize(): array { return ['y' => 7]; } function __unserialize(array $d): void { $this->x = $d['y']; } }
$p = new P; $p->self = $p;
$v = ['p' => $p, 's' => new S, 'arr' => [1, 'two', 3.0]];
$v['ref'] = &$v['arr'];
var_dump(ini_get('nocopy.serializer'));             // string(8) "igbinary"
var_dump(nocopy_put('k', $v, 0, true));             // bool(true)
$r = nocopy_get('k');
var_dump($r['p']->self === $r['p'], $r['s']->x, $r['arr'] === [1, 'two', 3.0]); // true, int(7), true
$r['ref'][] = 4; var_dump(count($r['arr']));        // int(4): reference kept
apcu_store('o', new ArrayObject([1, 2]));
var_dump(apcu_fetch('o')->count());                 // int(2)
var_dump(apcu_inc('o'));                            // bool(false): serialized entries are not counters
```
`tests/017_serializer_fallback.phpt`. INI: `nocopy.serializer=igbinary`, no igbinary loaded. A second section with `nocopy.serializer=msgpack` belongs in a separate test file, `017b_serializer_unknown.phpt`, with the same expectations.
EXPECTF: a startup `Warning: ... nocopy: igbinary is not loaded, using nocopy.serializer=php` (and `nocopy: unknown nocopy.serializer "msgpack", using php`); `ini_get('nocopy.serializer')` is `"php"`; an object round-trips through `nocopy_put(..., serialize: true)`.
`tests/018_apc_serializer_alias.phpt`. SKIPIF: no igbinary. INI: `extension=igbinary`, `apc.serializer=igbinary`.
Asserts `ini_get('nocopy.serializer') === 'igbinary'` and `ini_get('apc.serializer') === 'igbinary'`, plus one object round trip. Add a case with `apc.serializer=default` and an object round trip, expecting `ini_get('nocopy.serializer') === 'php'`.

- [ ] **Step 3: Run the tests to verify they fail**

Run: `docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/test.sh /build/tests/016_igbinary.phpt /build/tests/017_serializer_fallback.phpt /build/tests/017b_serializer_unknown.phpt /build/tests/018_apc_serializer_alias.phpt`
Expected: FAIL (`nocopy.serializer` is unknown, so `ini_get` returns false).

- [ ] **Step 4: Implement in `nocopy.c`**

- `PHP_INI_ENTRY("nocopy.serializer", "php", PHP_INI_SYSTEM, OnSerializer)` in `ini_nocopy`. `OnSerializer` parses `php`/`default` → php, `igbinary` → igbinary, anything else → E_CORE_WARNING + php, into a static `serializer_igbinary` bool.
- `PHP_INI_ENTRY("apc.serializer", "php", PHP_INI_SYSTEM, OnAlias)` in `ini_apc`. Add `"serializer"` to the `names` in `sync_aliases` (the loop bound becomes 4). After MINIT resolves the fallback, write the effective value back with `ini_put("nocopy.serializer", ...)` so `ini_get` shows it.
- The module entry switches to `STANDARD_MODULE_HEADER_EX` with deps `{ZEND_MOD_OPTIONAL("igbinary") ZEND_MOD_END}` so igbinary starts first.
- In MINIT, after INI registration, if igbinary was selected: `zend_module_entry *m = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("igbinary"))`. Resolve `igbinary_serialize`/`igbinary_unserialize` with `DL_FETCH_SYMBOL(m->handle ? m->handle : RTLD_DEFAULT, ...)` into static function pointers. If the module or either symbol is missing: E_CORE_WARNING `nocopy: igbinary is not loaded, using nocopy.serializer=php` and fall back.
- `serialize_value(zval *val, zval *out, bool *igb)`: with igbinary, call it and copy the result into a `zend_string` (`efree` the igbinary buffer). Otherwise use the current path. `build()` sets `e->flags = ENTRY_SERIALIZED | (igb ? ENTRY_IGBINARY : 0)`.
- `cache_unserialize(rv, buf, len, flags)`: for `ENTRY_IGBINARY`, use igbinary if resolved, else return a miss (`false`, `rv` NULL). Unwrap a top-level reference as today. Update the one caller in `fetch()` (:604) and any other caller (`grep cache_unserialize`).

- [ ] **Step 5: Run all tests**

Run: `docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/test.sh`
Expected: no FAIL lines, exit 0. 016 and 018 pass (not skipped). The ZTS image (`docker/Dockerfile.zts`, no igbinary) also passes, with 016 and 018 skipped.

- [ ] **Step 6: APCu suite with igbinary**

`scripts/apcu-suite.sh` appends `${=EXTRA}` to the `run-tests.php` arguments. Run it in the Docker image twice: default, and with `EXTRA="-d extension=igbinary -d apc.serializer=igbinary"`.
Expected: both runs show only the two known failures (apc54_014, ghbug335).

- [ ] **Step 7: README**

- Configuration table: add the row `nocopy.serializer` | `php` | `php` or `igbinary`: format for values stored serialized (objects, shared references). Arrays and scalars are never serialized. Alias: `apc.serializer`.
- The `apc.serializer` row in "Differences from APCu" (:164) becomes: used only for objects and shared references; arrays/scalars are stored natively.

- [ ] **Step 8: Commit**

```bash
git add nocopy.c docker/Dockerfile README.md scripts/apcu-suite.sh tests/016_igbinary.phpt tests/017_serializer_fallback.phpt tests/017b_serializer_unknown.phpt tests/018_apc_serializer_alias.phpt
git commit -m "nocopy.serializer=igbinary (alias apc.serializer): resolved via dlsym, per-entry flag, fallback to php with a startup warning"
```
