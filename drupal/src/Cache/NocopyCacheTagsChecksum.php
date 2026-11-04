<?php

declare(strict_types=1);

namespace Drupal\nocopy\Cache;

use Drupal\Core\Cache\CacheTagsChecksumInterface;
use Drupal\Core\Cache\CacheTagsChecksumPreloadInterface;
use Drupal\Core\Cache\CacheTagsChecksumTrait;
use Drupal\Core\Cache\CacheTagsInvalidatorInterface;
use Drupal\Core\Site\Settings;

/**
 * Cache tag checksums in nocopy: one random token per tag, fresh on every invalidation and whenever missing.
 *
 * Tokens instead of counters: there is no atomic increment, and a fresh random value differs from every
 * earlier one whoever wins a race, after eviction or after a restart. 2^48 keeps sums of up to 32,768 tags in int.
 */
class NocopyCacheTagsChecksum implements CacheTagsChecksumInterface, CacheTagsInvalidatorInterface, CacheTagsChecksumPreloadInterface {

  use CacheTagsChecksumTrait {
    getCurrentChecksum as traitGetCurrentChecksum;
    isValid as traitIsValid;
  }

  protected string $prefix;

  // Tag keys live under an epoch, so a web request that lost its journal position drops all tokens at once.
  protected string $epoch = '';

  protected ?float $replayedFor = NULL;

  public function __construct(string $root, string $sitePath, protected NocopyJournal $journal = new NocopyJournal()) {
    $this->prefix = Settings::getApcuPrefix('nocopy_tags', $root, $sitePath);
  }

  public function getTagKey(string $tag): string {
    $this->replay();
    return $this->prefix . '::' . $this->epoch . '::' . $tag;
  }

  // The trait answers from its tag cache first, so replay before it.
  public function getCurrentChecksum(array $tags) {
    $this->replay();
    return $this->traitGetCurrentChecksum($tags);
  }

  public function isValid($checksum, array $tags) {
    $this->replay();
    return $this->traitIsValid($checksum, $tags);
  }

  // Web requests: fresh tokens for tags that CLI processes invalidated, once per request.
  protected function replay(): void {
    if (NocopyJournal::shared()) {
      return;
    }
    $request = NocopyJournal::request();
    if ($this->replayedFor === $request || ($this->journal->writer && $this->replayedFor !== NULL)) {
      return;
    }
    $this->replayedFor = $request;
    $last = $this->journal->writer ? 0 : $this->journal->lastId();
    // [epoch, applied journal id]
    $state = nocopy_get($this->prefix . '::state', $hit);
    if (!$hit) {
      $state = [bin2hex(random_bytes(6)), $last];
      nocopy_put($this->prefix . '::state', $state);
    }
    elseif ($last > $state[1]) {
      foreach ($this->journal->since($state[1], NocopyJournal::TAGS) as $tags) {
        if ($tags === NULL) {
          $state[0] = bin2hex(random_bytes(6));
          break;
        }
        foreach ($tags as $tag) {
          nocopy_put($this->prefix . '::' . $state[0] . '::' . $tag, random_int(1, 2 ** 48));
        }
      }
      $state[1] = $last;
      nocopy_put($this->prefix . '::state', $state);
      $this->reset();
    }
    $this->epoch = $state[0];
  }

  // The trait hands over every preloaded tag at once: one lookup, one write for the missing ones.
  protected function getTagInvalidationCounts(array $tags) {
    $keys = [];
    foreach ($tags as $tag) {
      $keys[$this->getTagKey((string) $tag)] = $tag;
    }
    $found = nocopy_get_multiple(array_keys($keys));
    $counts = $new = [];
    foreach ($keys as $key => $tag) {
      $counts[$tag] = $found[$key] ?? ($new[$key] = random_int(1, 2 ** 48));
    }
    if ($new) {
      nocopy_put_multiple($new);
    }
    return $counts;
  }

  protected function doInvalidateTags(array $tags) {
    $new = [];
    foreach ($tags as $tag) {
      $new[$this->getTagKey($tag)] = random_int(1, 2 ** 48);
    }
    nocopy_put_multiple($new);
    $this->journal->record(NocopyJournal::TAGS, $tags);
  }

  // Only reached on invalidation, for the trait's transaction check.
  protected function getDatabaseConnection() {
    return \Drupal::database();
  }

}
