# Drupal Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Drupal module that makes nocopy the cache backend for all bins and the cache-tag checksum store on a single server, plus a benchmark that proves the gain against stock Drupal.

**Architecture:** `NocopyBackend` stores each cache item as a plain array via `nocopy_put()`, so reads of object-free data are zero-copy. It falls back to `$serialize: true` for values with objects.
`NocopyCacheTagsChecksum` uses core's `CacheTagsChecksumTrait` with one random 48-bit token per tag in nocopy. Wiring happens in `settings.php` plus an `example.services.yml`, the same pattern as `drupal/redis`.

**Tech Stack:** PHP 8.3+, Drupal 10.3+/11, PHPUnit kernel tests (`drupal/core-dev`), Docker (php-fpm, nginx, MariaDB, ab).

**Spec:** `docs/superpowers/specs/2026-11-04-drupal-integration-design.md` (Phase 2). Depends on `docs/superpowers/plans/2026-11-04-igbinary-serializer.md` (done first).

## Global Constraints

- Module lives in `drupal/`. Composer name `chrfritsch/nocopy-drupal`, type `drupal-module`, machine name `nocopy`, `core_version_requirement: ^10.3 || ^11`, PHP `>=8.3`, license MIT. Namespace `Drupal\nocopy`.
- Uses `nocopy_*` functions only, never `apcu_*`, so it works with `nocopy.apcu_compat=0`.
- Semantics identical to core backends: `$allow_invalid` returns invalidated and expired items, expire is checked against the request time, and the nocopy TTL is only a memory hint.
- A stale item must never look valid, after eviction, restart or invalidation.
- Tag tokens: random int in `[1, 2**48]`. Fresh on every invalidation, and fresh when missing.
- Single server only; the README says so.
- Comments: one short line, or none if the code is self-explanatory.

## Review Focus

- **Eviction of a tag token:** an item written before the eviction must read as invalid afterwards. Test in Task 2.
- **Item eviction under memory pressure:** `get()` returns FALSE and nothing throws. Test in Task 1 (a tiny `nocopy.shm_size` run of the eviction test).
- **Values containing objects,** including `Markup` inside render arrays and nested `stdClass`: they round-trip through the serialize fallback. Test in Task 1.
- **`deleteAll()` in one bin leaves other bins alone,** and a fresh backend instance (another request) sees the deletion. Test in Task 1.
- **Tags invalidated inside a database transaction** are delayed until commit (the trait's behavior). Covered by core's generic suite in Task 2.

---

### Task 1: `NocopyBackend` + Drupal test harness

**Files:**
- Create: `drupal/composer.json`, `drupal/nocopy.info.yml`, `drupal/nocopy.services.yml`
- Create: `drupal/src/Cache/NocopyBackend.php`, `drupal/src/Cache/NocopyBackendFactory.php`
- Create: `drupal/tests/src/Kernel/NocopyBackendTest.php`
- Create: `docker/Dockerfile.drupal`, `docker/drupal-test.sh`

**Interfaces:**
- Produces: `class NocopyBackend implements CacheBackendInterface`, `__construct(string $bin, string $sitePrefix, CacheTagsChecksumInterface $checksumProvider, TimeInterface $time)`.
- Produces: `class NocopyBackendFactory implements CacheFactoryInterface`, `__construct(string $root, string $sitePath, CacheTagsChecksumInterface $checksumProvider, TimeInterface $time)`. The prefix is `Settings::getApcuPrefix('nocopy_backend', $root, $sitePath)`.
- Produces: the service `cache.backend.nocopy` (arguments `['%app.root%', '%site.path%', '@cache_tags.invalidator.checksum', '@datetime.time']`).
- Produces: `docker/drupal-test.sh`, which runs the module's PHPUnit tests and is used by Task 2.

- [ ] **Step 1: Test harness**

`docker/Dockerfile.drupal`: `FROM nocopy-linux:8.5`. It adds `gd` (jpeg, freetype), `zip`, `pdo_mysql`, `mariadb-server`, `git`, `unzip` and composer (as in the spike Dockerfile: `apt-get` + `docker-php-ext-configure gd --with-jpeg --with-freetype && docker-php-ext-install gd zip pdo_mysql`). Do not install opcache, which is built in on 8.5.
`docker/drupal-test.sh` (inside the image, `/src` = repo, read-only):
1. `/src/docker/test.sh >/dev/null` builds `/build/modules/nocopy.so`.
2. If `/drupal/web` is missing, run `composer create-project drupal/recommended-project:^11 /drupal` and `composer require --dev drupal/core-dev` (`/drupal` is a volume, so this happens once).
3. `rm -rf /drupal/web/modules/contrib/nocopy && cp -r /src/drupal /drupal/web/modules/contrib/nocopy`.
4. For each of `""` and `"-d nocopy.request_scope=1"`, run `SIMPLETEST_DB=sqlite://localhost//tmp/t.sqlite php -d extension=/build/modules/nocopy.so -d extension=igbinary -d nocopy.serializer=igbinary $mode vendor/bin/phpunit -c web/core web/modules/contrib/nocopy/tests`.

Exit non-zero on failure. Usage line at the top: `docker run --rm -v "$PWD":/src:ro -v nocopy-drupal-test:/drupal nocopy-drupal /src/docker/drupal-test.sh`.

- [ ] **Step 2: Write the failing test**

`NocopyBackendTest extends GenericCacheBackendUnitTestBase`. `createCacheBackend($bin)` returns `new NocopyBackend($bin, 'test' . $this->randomMachineName(), \Drupal::service('cache_tags.invalidator.checksum'), \Drupal::service('datetime.time'))`, keeping the prefix in a property so tests can build a second instance. Extra tests:
```php
public function testObjectsRoundTrip(): void {
  $b = $this->getCacheBackend();
  $data = ['#markup' => Markup::create('<b>x</b>'), 'o' => (object) ['a' => [1]], 'arr' => [1, 2]];
  $b->set('obj', $data);
  $item = $b->get('obj');
  $this->assertInstanceOf(Markup::class, $item->data['#markup']);
  $this->assertSame('<b>x</b>', (string) $item->data['#markup']);
  $this->assertSame([1], $item->data['o']->a);
}
public function testDeleteAllIsolatedAndSeenByNewInstance(): void {
  $a = $this->getCacheBackend('bin_a'); $b = $this->getCacheBackend('bin_b');
  $a->set('k', 1); $b->set('k', 2);
  $a->deleteAll();
  $this->assertFalse($this->createCacheBackend('bin_a')->get('k'));  // same prefix, fresh instance
  $this->assertSame(2, $b->get('k')->data);
}
public function testReturnsPlainArrayDataUnchanged(): void {
  $b = $this->getCacheBackend(); $big = range(1, 1000);
  $b->set('big', $big);
  $this->assertSame($big, $b->get('big')->data);
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `docker build -t nocopy-drupal -f docker/Dockerfile.drupal docker && docker run --rm -v "$PWD":/src:ro -v nocopy-drupal-test:/drupal nocopy-drupal /src/docker/drupal-test.sh`
Expected: FAIL, class `Drupal\nocopy\Cache\NocopyBackend` not found.

- [ ] **Step 4: Implement module files and `NocopyBackend`**

- Key: `"$sitePrefix::$bin::" . $this->binVersion() . "::$cid"`. `binVersion()` reads `nocopy_get("$sitePrefix::$bin::version")` on every call (another request may have bumped it), defaulting to `'0'`.
- Stored value: `[$data, $created, $expire, $tagsString, $checksum]`, with `$created = round(microtime(TRUE), 3)`, tags `implode(' ', array_unique($tags))`, and checksum `$checksumProvider->getCurrentChecksum($tags)`.
- `set()` calls `nocopy_put($key, $item, $ttl)`. On `\TypeError` or `\ValueError` (objects, or too deep) it retries with `serialize: true`. `$ttl` is `0` for `CACHE_PERMANENT`, else `max(1, $expire - $requestTime) + 3600`.
- `prepareItem(array $raw, bool $allowInvalid): object|false` builds the `stdClass` (`cid`, `data`, `created`, `expire`, `tags` as an array, `checksum`, `valid`) like core's `ApcuBackend::prepareItem()`.
- `getMultiple()` registers the tags of all hits with `CacheTagsChecksumPreloadInterface` before validating, as `ApcuBackend` does.
- `invalidateMultiple()` re-puts each raw item with index 2 set to `requestTime - 1`; the checksum is kept.
- `deleteAll()`, `invalidateAll()` and `removeBin()` write a new version, `bin2hex(random_bytes(6))`.
- `garbageCollection()` does nothing.

- [ ] **Step 5: Run the tests to verify they pass**

Run the Step 3 command. Expected: OK in both modes (copy and `request_scope=1`).
Then run once more with `-d nocopy.shm_size=1M` added (a temporary env var `EXTRA` in `drupal-test.sh`, passed to php) to exercise eviction. Expected: OK.

- [ ] **Step 6: Commit**

```bash
git add drupal docker/Dockerfile.drupal docker/drupal-test.sh
git commit -m "drupal: NocopyBackend (plain-array items, serialize fallback for objects, bin versions) + Drupal kernel test harness"
```

### Task 2: `NocopyCacheTagsChecksum`

**Files:**
- Create: `drupal/src/Cache/NocopyCacheTagsChecksum.php`, `drupal/example.services.yml`
- Create: `drupal/tests/src/Kernel/NocopyCacheTagsChecksumTest.php`
- Modify: `drupal/tests/src/Kernel/NocopyBackendTest.php` (run the generic suite with the nocopy checksum service)

**Interfaces:**
- Consumes: `NocopyBackend` (Task 1), `docker/drupal-test.sh` (Task 1).
- Produces: `class NocopyCacheTagsChecksum implements CacheTagsChecksumInterface, CacheTagsInvalidatorInterface, CacheTagsChecksumPreloadInterface` with `use CacheTagsChecksumTrait`, `__construct(string $root, string $sitePath)`, prefix `Settings::getApcuPrefix('nocopy_tags', $root, $sitePath)`, and `public function getTagKey(string $tag): string` (`"$prefix::$tag"`).
- Produces: `drupal/example.services.yml`, which overrides `cache_tags.invalidator.checksum` with this class (arguments `['%app.root%', '%site.path%']`, tag `cache_tags_invalidator`).

- [ ] **Step 1: Write the failing tests**

`NocopyCacheTagsChecksumTest extends KernelTestBase`, with the service built directly (`new NocopyCacheTagsChecksum($this->root, 'sites/test' . $this->randomMachineName())`):
```php
public function testInvalidationChangesChecksum(): void {
  $c = $this->checksum->getCurrentChecksum(['a', 'b']);
  $this->checksum->invalidateTags(['a']); $this->checksum->reset();
  $this->assertFalse($this->checksum->isValid($c, ['a', 'b']));
}
public function testUnrelatedInvalidationKeepsValid(): void {
  $c = $this->checksum->getCurrentChecksum(['a']);
  $this->checksum->invalidateTags(['z']); $this->checksum->reset();
  $this->assertTrue($this->checksum->isValid($c, ['a']));
}
public function testEvictedTokenInvalidates(): void {
  $c = $this->checksum->getCurrentChecksum(['a']);
  nocopy_delete($this->checksum->getTagKey('a')); $this->checksum->reset();
  $this->assertFalse($this->checksum->isValid($c, ['a']));
}
public function testTokensAreFreshAndInRange(): void {
  $seen = [];
  for ($i = 0; $i < 1000; $i++) {
    $this->checksum->invalidateTags(['t']); $this->checksum->reset();
    $t = nocopy_get($this->checksum->getTagKey('t'));
    $this->assertIsInt($t); $this->assertGreaterThanOrEqual(1, $t); $this->assertLessThanOrEqual(2 ** 48, $t);
    $seen[$t] = TRUE;
  }
  $this->assertCount(1000, $seen);
}
```
In `NocopyBackendTest`, override `register(ContainerBuilder $container)` to set `cache_tags.invalidator.checksum` to `NocopyCacheTagsChecksum` (arguments `['%app.root%', '%site.path%']`, tag `cache_tags_invalidator`). The whole generic suite (tag invalidation, `$allow_invalid`) then runs against nocopy tags.

- [ ] **Step 2: Run them to verify they fail**

Run: `docker run --rm -v "$PWD":/src:ro -v nocopy-drupal-test:/drupal nocopy-drupal /src/docker/drupal-test.sh`
Expected: FAIL, class `NocopyCacheTagsChecksum` not found.

- [ ] **Step 3: Implement `NocopyCacheTagsChecksum`**

- `getTagInvalidationCounts(array $tags): array`: for each tag, `nocopy_get($this->getTagKey($tag), $hit)`. On a miss, write `random_int(1, 2 ** 48)` with `nocopy_put` and use it.
- `doInvalidateTags(array $tags)`: `nocopy_put` a fresh `random_int(1, 2 ** 48)` per tag.
- `getDatabaseConnection()`: `\Drupal::database()` (only reached on invalidation, as in the Redis module).

- [ ] **Step 4: Run the tests to verify they pass**

Run the Step 2 command. Expected: OK in both modes.

- [ ] **Step 5: Commit**

```bash
git add drupal
git commit -m "drupal: NocopyCacheTagsChecksum (random 48-bit token per tag, fresh on invalidation and on eviction) + example.services.yml"
```

### Task 3: Benchmark

**Files:**
- Create: `docker/drupal-bench.sh`
- Create: `bench/drupal/settings.stock.php`, `bench/drupal/settings.fast.php`, `bench/drupal/settings.all.php`, `bench/drupal/fast.services.yml`
- Create: `bench/drupal/editor.sh` (the spike's editor loop: edit form → POST save → view → `/admin/content` → `/node/add/article`)
- Create: `bench/results-drupal/` (committed output)

**Interfaces:**
- Consumes: `cache.backend.nocopy` (Task 1), `drupal/example.services.yml` (Task 2), `nocopy.serializer` (igbinary plan), the `nocopy-drupal` image (Task 1).

- [ ] **Step 1: Site setup inside `docker/drupal-bench.sh`** (`/site` volume, done once)

Start MariaDB (buffer pool 512M, otherwise defaults). Run `composer create-project drupal/recommended-project:^11` and require `drush/drush drupal/devel`. Then `drush si standard --db-url=mysql://...`, `drush recipe core/recipes/standard` (11.4's profile alone has no content types), `drush en devel_generate`, `devel-generate:users 20`, `devel-generate:terms 50 --bundles=tags`, `devel-generate:content 2000 --bundles=article,page`, `devel-generate:menus 2 30`.
Copy `/src/drupal` to `web/modules/contrib/nocopy` and enable it. `settings.php` ends with `include $app_root . '/' . $site_path . '/settings.bench.php';`, the file each config copies into place.

- [ ] **Step 2: Config files**

- `settings.stock.php`: empty (core defaults: database + APCu chainedfast).
- `settings.fast.php`: `$settings['container_yamls'][] = __DIR__ . '/fast.services.yml';`. `fast.services.yml` redefines `cache.backend.chainedfast` with arguments `['@settings', null, 'cache.backend.nocopy']`.
- `settings.all.php`:
  - `$settings['container_yamls'][] = $app_root . '/modules/contrib/nocopy/example.services.yml'`
  - `$settings['cache']['default'] = 'cache.backend.nocopy'`
  - `$settings['cache']['bins']` for `bootstrap`, `config`, `discovery` = `'cache.backend.nocopy'` (bin defaults outrank `default` in `CacheFactory::get()`)
  - `$settings['bootstrap_container_definition']` modeled on the Redis module README: services `cache.container` (class `Drupal\nocopy\Cache\NocopyBackend`, arguments `['container', Settings::getApcuPrefix('nocopy_backend', $app_root, $site_path), '@cache_tags_provider.container', '@datetime.time']`), `cache_tags_provider.container` (`NocopyCacheTagsChecksum`, `[$app_root, $site_path]`), `datetime.time` (`Drupal\Component\Datetime\Time`).

- [ ] **Step 3: The run loop in `docker/drupal-bench.sh`**

Configs, each with `pm = static`, `pm.max_children = ${WORKERS:-8}` and opcache with `validate_timestamps=0`:
1. `stock`: apcu, `settings.stock.php`
2. `stock-igbinary`: apcu + `apc.serializer=igbinary`
3. `nocopy-fast`: nocopy + `nocopy.serializer=igbinary`, `settings.fast.php`
4. `nocopy-all`: nocopy + `nocopy.serializer=igbinary`, `settings.all.php`

Shared memory is 256M in every config. Per config:
- `drush cr`, start php-fpm, warm up, log in via `drush uli` (cookie jar).
- **Anonymous and logged-in:** for `/`, `/node/42`, `/taxonomy/term/3`, take the median of 3 `ab -n ${REQS:-5000} -c ${CONC:-8}` runs (`-C` session cookie for logged-in), recording req/s, p50 and p99.
- **Editor:** `bench/drupal/editor.sh` runs 50 saves while `ab -c 4 -C` loads `/node/42` in the background. Record the editor step p50 and the reader req/s. Every save must return 303, or the run fails.
- **Database queries per request:** the delta of `SHOW GLOBAL STATUS LIKE 'Questions'` around one `ab` run, divided by its requests.
- **Cold start:** restart php-fpm, request logged-in `/` 100 times sequentially, and record the request number at which latency first stays within 1.5× the warmed p50 for 10 requests.

Output: one TSV per config, `bench/results-drupal/<config>.tsv`, with columns `traffic url req_s p50 p99 queries_per_req`, plus `editor.tsv` and `coldstart.tsv`.
Usage line: `docker run --rm -v "$PWD":/src:ro -v nocopy-drupal-site:/site nocopy-drupal /src/docker/drupal-bench.sh`.

- [ ] **Step 4: Run it**

Run the usage line. Expected: four config TSVs, no failed requests, all saves 303, and `nocopy-all` showing fewer database queries per request than `stock`. If `nocopy-all` is not faster than `stock-igbinary` for logged-in or editor traffic, stop and report the numbers. Do not tune the benchmark toward a result.

- [ ] **Step 5: Commit**

```bash
git add docker/drupal-bench.sh bench/drupal bench/results-drupal
git commit -m "bench: Drupal 11 single-server benchmark (stock, igbinary, nocopy fast layer, nocopy all bins; anon, logged-in, editor, cold start)"
```

### Task 4: README

**Files:**
- Modify: `README.md` (Features list, benchmark table, new "### Drupal" section after "### Symfony")
- Create: `drupal/README.md` (install + settings snippet; the Composer package page shows it)

- [ ] **Step 1: Write the Drupal section**

- Install: `composer config repositories.nocopy path /path/to/nocopy/drupal` and `composer require chrfritsch/nocopy-drupal:@dev`, then `drush en nocopy`.
- The `settings.php` lines from `bench/drupal/settings.all.php`, verbatim.
- Trade-offs in plain words: the cache is empty after a php-fpm restart (give the measured cold-start number), size `nocopy.shm_size` from `nocopy_stats()['used']` after warm-up (give the benchmark's number), single server only, and the fast-layer-only alternative (`settings.fast.php`).
- Benchmark table: Drupal rows (logged-in `/node/42`, editor save p50, anonymous `/`) for `stock`, `stock-igbinary` and `nocopy-all`, from `bench/results-drupal/`, with the reproduce command.

- [ ] **Step 2: Check**

Every number in the README matches a TSV in `bench/results-drupal/`, and every snippet matches the files it was copied from (`diff` the PHP lines by eye).

- [ ] **Step 3: Commit**

```bash
git add README.md drupal/README.md
git commit -m "README: Drupal section (install, settings, trade-offs) and Drupal benchmark rows"
```
