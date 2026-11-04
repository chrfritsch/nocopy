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
 */
class NocopyBackend implements CacheBackendInterface {

  // Positions in the stored array.
  private const DATA = 0, CREATED = 1, EXPIRE = 2, TAGS = 3, CHECKSUM = 4;

  // Invalidated items outlive their expiry by this long for $allow_invalid readers, then nocopy may drop them.
  private const TTL_SLACK = 3600;

  public function __construct(
    protected string $bin,
    protected string $sitePrefix,
    protected CacheTagsChecksumInterface $checksumProvider,
    protected TimeInterface $time,
    // NULL: no journal (fast layer, the database is the source of truth).
    protected ?NocopyJournal $journal = new NocopyJournal(),
  ) {}

  protected ?float $replayedFor = NULL;

  protected function binKey(string $name): string {
    return $this->sitePrefix . '::' . $this->bin . '::' . $name;
  }

  // [version, invalidated_at], read on every call: another request may have cleared the bin.
  // Either key lost (new bin, eviction, nocopy.ttl): start a fresh version. That only costs misses, a default could revive deleted items.
  protected function state(): array {
    $this->replay();
    $version = nocopy_get($this->binKey('version'), $hit);
    $invalidated = nocopy_get($this->binKey('invalidated_at'), $invalidatedHit);
    if (!$hit || !$invalidatedHit) {
      $version = bin2hex(random_bytes(6));
      nocopy_put($this->binKey('version'), $version);
      nocopy_put($this->binKey('invalidated_at'), [$version, 0.0]);
      return [$version, 0.0];
    }
    // invalidateAll() of an older version does not apply after deleteAll().
    return [$version, $invalidated[0] === $version ? $invalidated[1] : 0.0];
  }

  // Web requests: apply what CLI processes wrote since this bin last looked, once per request.
  protected function replay(): void {
    if (!$this->journal || $this->journal->writer || $this->replayedFor === NocopyJournal::request()) {
      return;
    }
    $this->replayedFor = NocopyJournal::request();
    $last = $this->journal->lastId();
    $applied = nocopy_get($this->binKey('applied'), $hit);
    if (!$hit) {
      // Position unknown: start over.
      $this->deleteAll();
    }
    elseif ($last > $applied) {
      foreach ($this->journal->since($applied, $this->bin) as $cids) {
        $cids === NULL ? $this->deleteAll() : $this->deleteMultiple($cids);
      }
    }
    else {
      return;
    }
    nocopy_put($this->binKey('applied'), $last);
  }

  protected function key(string $cid, ?string $version = NULL): string {
    return $this->binKey(($version ?? $this->state()[0]) . '::' . $cid);
  }

  public function get($cid, $allow_invalid = FALSE) {
    $cids = [$cid];
    return $this->getMultiple($cids, $allow_invalid)[$cid] ?? FALSE;
  }

  public function getMultiple(&$cids, $allow_invalid = FALSE) {
    $raw = [];
    [$version, $invalidatedAt] = $this->state();
    foreach ($cids as $cid) {
      $item = nocopy_get($this->key((string) $cid, $version));
      if (is_array($item)) {
        $raw[$cid] = $item;
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
      if ($prepared = $this->prepareItem($cid, $item, $allow_invalid, $invalidatedAt)) {
        $cache[$cid] = $prepared;
      }
    }
    $cids = array_values(array_diff($cids, array_keys($cache)));
    return $cache;
  }

  protected function prepareItem(string|int $cid, array $item, bool $allow_invalid, float $invalidatedAt): object|false {
    $cache = new \stdClass();
    $cache->cid = $cid;
    $cache->data = $item[self::DATA];
    $cache->created = $item[self::CREATED];
    $cache->expire = $item[self::EXPIRE];
    $cache->tags = $item[self::TAGS] === '' ? [] : explode(' ', $item[self::TAGS]);
    $cache->checksum = $item[self::CHECKSUM];
    $cache->valid = ($cache->expire == CacheBackendInterface::CACHE_PERMANENT || $cache->expire >= $this->time->getRequestTime())
      && $cache->created > $invalidatedAt
      && $this->checksumProvider->isValid($cache->checksum, $cache->tags);
    return $allow_invalid || $cache->valid ? $cache : FALSE;
  }

  public function set($cid, $data, $expire = CacheBackendInterface::CACHE_PERMANENT, array $tags = []) {
    $this->store($cid, $data, $expire, $tags);
    $this->journal?->record($this->bin, [$cid]);
  }

  protected function store($cid, $data, $expire, array $tags): void {
    assert(Inspector::assertAllStrings($tags), 'Cache tags must be strings.');
    $tags = array_unique($tags);
    $this->put((string) $cid, [$data, round(microtime(TRUE), 3), $expire, implode(' ', $tags), $this->checksumProvider->getCurrentChecksum($tags)]);
  }

  protected function put(string $cid, array $item): void {
    $expire = $item[self::EXPIRE];
    $ttl = $expire == CacheBackendInterface::CACHE_PERMANENT ? 0 : max(1, $expire - $this->time->getRequestTime()) + self::TTL_SLACK;
    $key = $this->key($cid);
    try {
      nocopy_put($key, $item, $ttl);
    }
    catch (\TypeError | \ValueError) {
      // Objects or too deep: stored serialized.
      nocopy_put($key, $item, $ttl, TRUE);
    }
  }

  public function setMultiple(array $items) {
    foreach ($items as $cid => $item) {
      $this->store($cid, $item['data'], $item['expire'] ?? CacheBackendInterface::CACHE_PERMANENT, $item['tags'] ?? []);
    }
    $this->journal?->record($this->bin, array_keys($items));
  }

  public function delete($cid) {
    $this->deleteMultiple([$cid]);
  }

  public function deleteMultiple(array $cids) {
    foreach ($cids as $cid) {
      nocopy_delete($this->key((string) $cid));
    }
    $this->journal?->record($this->bin, $cids);
  }

  // Old entries leave through eviction.
  public function deleteAll() {
    nocopy_put($this->binKey('version'), bin2hex(random_bytes(6)));
    $this->journal?->record($this->bin, NULL);
  }

  public function invalidate($cid) {
    $this->invalidateMultiple([$cid]);
  }

  public function invalidateMultiple(array $cids) {
    $past = $this->time->getRequestTime() - 1;
    foreach ($cids as $cid) {
      $item = nocopy_get($this->key((string) $cid));
      if (is_array($item)) {
        $item[self::EXPIRE] = $past;
        $this->put((string) $cid, $item);
      }
    }
    $this->journal?->record($this->bin, $cids);
  }

  public function invalidateAll() {
    @trigger_error("CacheBackendInterface::invalidateAll() is deprecated in drupal:11.2.0 and is removed from drupal:12.0.0. Use CacheBackendInterface::deleteAll() or cache tag invalidation instead. See https://www.drupal.org/node/3500622", E_USER_DEPRECATED);
    // Rounded like 'created', so every item created up to now compares <=.
    nocopy_put($this->binKey('invalidated_at'), [$this->state()[0], round(microtime(TRUE), 3)]);
    $this->journal?->record($this->bin, NULL);
  }

  public function garbageCollection() {
  }

  public function removeBin() {
    $this->deleteAll();
  }

}
