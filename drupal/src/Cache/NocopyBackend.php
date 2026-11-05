<?php

declare(strict_types=1);

namespace Drupal\nocopy\Cache;

use Drupal\Component\Assertion\Inspector;
use Drupal\Component\Datetime\TimeInterface;
use Drupal\Core\Cache\CacheBackendInterface;
use Drupal\Core\Cache\CacheTagsChecksumInterface;
use Drupal\Core\Cache\CacheTagsChecksumPreloadInterface;

/**
 * Cache bin in nocopy shared memory. Items are plain arrays, so object-free data is read without a copy.
 *
 * Every PHP process of the site must share one cache (nocopy.shm_name on one server, nocopy.valkey on several):
 * writes by Drush and cron then reach php-fpm by themselves. Checked once per process.
 */
class NocopyBackend implements CacheBackendInterface {

  // Positions in the stored array. VERSION: the bin's version when written, so deleteAll() needs no new keys.
  private const DATA = 0, CREATED = 1, EXPIRE = 2, TAGS = 3, CHECKSUM = 4, VERSION = 5;

  // Invalidated items outlive their expiry by this long for $allow_invalid readers, then nocopy may drop them.
  private const TTL_SLACK = 3600;

  // With nocopy.valkey: permanent items expire after a year, so Valkey's volatile-* eviction can drop them
  // but never tag tokens, bin versions or these small bins (no TTL).
  private const PERM_TTL = 31536000, PERMANENT_BINS = ['container', 'bootstrap', 'config', 'discovery'];

  // cid => TRUE: deleted inside a transaction, removed after the commit (a concurrent request could refill them from uncommitted data).
  protected array $delayedDeletions = [];

  public function __construct(
    protected string $bin,
    protected string $sitePrefix,
    protected CacheTagsChecksumInterface $checksumProvider,
    protected TimeInterface $time,
    // FALSE: the fast layer (chainedfast), where the database is the source of truth and a cache per process will do.
    bool $requireShared = TRUE,
  ) {
    if ($requireShared) {
      self::requireShared();
    }
  }

  private static bool $shared = FALSE;

  // A process with a cache of its own would serve what other processes already changed.
  public static function requireShared(): void {
    if (self::$shared) {
      return;
    }
    $with = function_exists('nocopy_shared_with') ? nocopy_shared_with() : 'none (the nocopy extension is not loaded)';
    if ($with !== 'host' && $with !== 'valkey') {
      throw new \RuntimeException(sprintf('nocopy: this %s process does not share the cache with the rest of the site (nocopy_shared_with(): %s). Set nocopy.shm_name (one server) or nocopy.valkey (several) in the php.ini of every PHP that runs this site, and run Drush and cron as the user that starts php-fpm.', PHP_SAPI, $with));
    }
    self::$shared = TRUE;
  }

  protected function key(string $cid): string {
    return $this->sitePrefix . '::' . $this->bin . '::' . $cid;
  }

  // Read on every call: another request may have cleared the bin.
  // Lost (new bin, eviction): a fresh version. That only costs misses, a default could revive deleted items.
  protected function version(): string {
    $version = nocopy_get($this->versionKey(), $hit);
    if (!$hit) {
      $version = bin2hex(random_bytes(6));
      nocopy_put($this->versionKey(), $version);
    }
    return $version;
  }

  // One colon: no cid key can match it.
  public function versionKey(): string {
    return $this->sitePrefix . '::' . $this->bin . ':version';
  }

  public function get($cid, $allow_invalid = FALSE) {
    $cids = [$cid];
    return $this->getMultiple($cids, $allow_invalid)[$cid] ?? FALSE;
  }

  public function getMultiple(&$cids, $allow_invalid = FALSE) {
    $raw = [];
    $version = $this->version();
    $keys = [];
    foreach ($cids as $cid) {
      $keys[$this->key((string) $cid)] = $cid;
    }
    // With nocopy.valkey: every local miss in one round trip.
    $found = nocopy_get_multiple(array_keys($keys));
    foreach ($keys as $key => $cid) {
      if (($found[$key][self::VERSION] ?? NULL) === $version && !isset($this->delayedDeletions[$cid])) {
        $raw[$cid] = $found[$key];
      }
    }
    if ($raw && $this->checksumProvider instanceof CacheTagsChecksumPreloadInterface) {
      $tags = [];
      foreach ($raw as $item) {
        if ($item[self::TAGS] !== '') {
          $tags[] = explode(' ', $item[self::TAGS]);
        }
      }
      $this->checksumProvider->registerCacheTagsForPreload(array_merge(...$tags));
    }
    $cache = [];
    foreach ($raw as $cid => $item) {
      if ($prepared = $this->prepareItem($cid, $item, $allow_invalid)) {
        $cache[$cid] = $prepared;
      }
    }
    $cids = array_values(array_diff($cids, array_keys($cache)));
    return $cache;
  }

  protected function prepareItem(string|int $cid, array $item, bool $allow_invalid): object|false {
    $cache = new \stdClass();
    $cache->cid = $cid;
    $cache->data = $item[self::DATA];
    $cache->created = $item[self::CREATED];
    $cache->expire = $item[self::EXPIRE];
    $cache->tags = $item[self::TAGS] === '' ? [] : explode(' ', $item[self::TAGS]);
    $cache->checksum = $item[self::CHECKSUM];
    $cache->valid = ($cache->expire == CacheBackendInterface::CACHE_PERMANENT || $cache->expire >= $this->time->getRequestTime())
      && $this->checksumProvider->isValid($cache->checksum, $cache->tags);
    return $allow_invalid || $cache->valid ? $cache : FALSE;
  }

  public function set($cid, $data, $expire = CacheBackendInterface::CACHE_PERMANENT, array $tags = []) {
    $this->setMultiple([$cid => ['data' => $data, 'expire' => $expire, 'tags' => $tags]]);
  }

  protected function item($data, $expire, array $tags, string $version): array {
    $tags = array_unique($tags);
    return [$data, round(microtime(TRUE), 3), $expire, implode(' ', $tags), $this->checksumProvider->getCurrentChecksum($tags), $version];
  }

  protected function ttl($expire): int {
    if ($expire == CacheBackendInterface::CACHE_PERMANENT) {
      return nocopy_shared_with() === 'valkey' && !in_array($this->bin, self::PERMANENT_BINS, TRUE) ? self::PERM_TTL : 0;
    }
    return max(1, $expire - $this->time->getRequestTime()) + self::TTL_SLACK;
  }

  // One nocopy_put_multiple() per TTL: with nocopy.valkey one pipeline each. Tags first, in one lookup.
  // $serialize only serializes items holding objects; the rest stay zero-copy.
  public function setMultiple(array $items) {
    $tags = array_merge(...array_map(fn ($item) => $item['tags'] ?? [], array_values($items)));
    assert(Inspector::assertAllStrings($tags), 'Cache tags must be strings.');
    $this->checksumProvider->getCurrentChecksum($tags);
    $version = $this->version();
    $byTtl = [];
    foreach ($items as $cid => $item) {
      $entry = $this->item($item['data'], $item['expire'] ?? CacheBackendInterface::CACHE_PERMANENT, $item['tags'] ?? [], $version);
      $byTtl[$this->ttl($entry[self::EXPIRE])][$this->key((string) $cid)] = $entry;
    }
    foreach ($byTtl as $ttl => $entries) {
      nocopy_put_multiple($entries, $ttl, TRUE);
    }
  }

  public function delete($cid) {
    $this->deleteMultiple([$cid]);
  }

  public function deleteMultiple(array $cids) {
    $db = \Drupal::hasContainer() && \Drupal::hasService('database') ? \Drupal::database() : NULL;
    if (!$db?->inTransaction()) {
      $this->doDelete($cids);
      return;
    }
    if (!$this->delayedDeletions) {
      $db->transactionManager()->addPostTransactionCallback([$this, 'postRootTransactionCommit']);
    }
    $this->delayedDeletions += array_fill_keys(array_map('strval', $cids), TRUE);
  }

  // Rolled back: the items are as valid as before the transaction.
  public function postRootTransactionCommit(bool $success): void {
    $cids = array_keys($this->delayedDeletions);
    $this->delayedDeletions = [];
    if ($success && $cids) {
      $this->doDelete($cids);
    }
  }

  protected function doDelete(array $cids): void {
    foreach ($cids as $cid) {
      nocopy_delete($this->key((string) $cid));
    }
  }

  // Old items stay until rewritten (same key), evicted or expired.
  public function deleteAll() {
    nocopy_put($this->versionKey(), bin2hex(random_bytes(6)));
  }

  public function invalidate($cid) {
    $this->invalidateMultiple([$cid]);
  }

  public function invalidateMultiple(array $cids) {
    $past = $this->time->getRequestTime() - 1;
    $version = $this->version();
    foreach ($cids as $cid) {
      $key = $this->key((string) $cid);
      $item = nocopy_get($key);
      if (($item[self::VERSION] ?? NULL) === $version) {
        $item[self::EXPIRE] = $past;
        nocopy_put($key, $item, $this->ttl($past), TRUE);
      }
    }
  }

  // As a deleteAll(), like the Redis module: deprecated, and one marker less to read on every call.
  public function invalidateAll() {
    @trigger_error("CacheBackendInterface::invalidateAll() is deprecated in drupal:11.2.0 and is removed from drupal:12.0.0. Use CacheBackendInterface::deleteAll() or cache tag invalidation instead. See https://www.drupal.org/node/3500622", E_USER_DEPRECATED);
    $this->deleteAll();
  }

  public function garbageCollection() {
  }

  public function removeBin() {
    $this->deleteAll();
  }

}
