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

  // With nocopy.valkey: permanent items expire after a year, so Valkey's volatile-* eviction can drop them
  // but never tag tokens, bin state or these small bins (no TTL). Not from Settings: the container bin is read before it exists.
  private const PERM_TTL = 31536000, PERMANENT_BINS = ['container', 'bootstrap', 'config', 'discovery'];

  // cid => TRUE: deleted inside a transaction, removed after the commit (a concurrent request could refill them from uncommitted data).
  protected array $delayedDeletions = [];

  public function __construct(
    protected string $bin,
    protected string $sitePrefix,
    protected CacheTagsChecksumInterface $checksumProvider,
    protected TimeInterface $time,
    // NULL: no journal (fast layer, the database is the source of truth).
    protected ?NocopyJournal $journal = new NocopyJournal(),
  ) {
    if (NocopyJournal::shared()) {
      $this->journal = NULL;
    }
  }

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
    $keys = [];
    foreach ($cids as $cid) {
      $keys[$this->key((string) $cid, $version)] = $cid;
    }
    // With nocopy.valkey: every local miss in one round trip.
    $found = nocopy_get_multiple(array_keys($keys));
    foreach ($keys as $key => $cid) {
      if (is_array($found[$key] ?? NULL) && !isset($this->delayedDeletions[$cid])) {
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
    $this->put((string) $cid, $this->item($data, $expire, $tags));
  }

  protected function item($data, $expire, array $tags): array {
    assert(Inspector::assertAllStrings($tags), 'Cache tags must be strings.');
    $tags = array_unique($tags);
    return [$data, round(microtime(TRUE), 3), $expire, implode(' ', $tags), $this->checksumProvider->getCurrentChecksum($tags)];
  }

  protected function ttl($expire): int {
    if ($expire == CacheBackendInterface::CACHE_PERMANENT) {
      return NocopyJournal::shared() && !in_array($this->bin, self::PERMANENT_BINS, TRUE) ? self::PERM_TTL : 0;
    }
    return max(1, $expire - $this->time->getRequestTime()) + self::TTL_SLACK;
  }

  protected function put(string $cid, array $item): void {
    $ttl = $this->ttl($item[self::EXPIRE]);
    $key = $this->key($cid);
    try {
      nocopy_put($key, $item, $ttl);
    }
    catch (\TypeError | \ValueError) {
      // Objects or too deep: stored serialized.
      nocopy_put($key, $item, $ttl, TRUE);
    }
  }

  // One nocopy_put_multiple() per TTL: with nocopy.valkey one pipeline each. Tags first, in one lookup.
  public function setMultiple(array $items) {
    $tags = array_merge(...array_map(fn ($item) => $item['tags'] ?? [], array_values($items)));
    assert(Inspector::assertAllStrings($tags), 'Cache tags must be strings.');
    $this->checksumProvider->getCurrentChecksum($tags);
    $byTtl = [];
    foreach ($items as $cid => $item) {
      $entry = $this->item($item['data'], $item['expire'] ?? CacheBackendInterface::CACHE_PERMANENT, $item['tags'] ?? []);
      $byTtl[$this->ttl($entry[self::EXPIRE])][$this->key((string) $cid)] = $entry;
    }
    foreach ($byTtl as $ttl => $entries) {
      nocopy_put_multiple($entries, $ttl, TRUE);
    }
    $this->journal?->record($this->bin, array_keys($items));
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
