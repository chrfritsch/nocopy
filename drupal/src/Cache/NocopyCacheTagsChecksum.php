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
 * Tokens instead of counters: an evicted counter restarts at 0 and could match an old checksum again; a fresh random
 * value differs from every earlier one, after eviction or a restart. 2^48 keeps sums of up to 32,768 tags in int.
 */
class NocopyCacheTagsChecksum implements CacheTagsChecksumInterface, CacheTagsInvalidatorInterface, CacheTagsChecksumPreloadInterface {

  use CacheTagsChecksumTrait {
    isValid as private traitIsValid;
  }

  protected string $prefix;

  public function __construct(string $root, string $sitePath) {
    NocopyBackend::requireShared();
    $this->prefix = ($prefix = Settings::get('nocopy_prefix')) ? $prefix . '.tags' : Settings::getApcuPrefix('nocopy_tags', $root, $sitePath);
  }

  // Tags all loaded, nothing delayed or to preload: just the sum.
  public function isValid($checksum, array $tags) {
    if ($this->delayedTags || $this->preloadTags) {
      return $this->traitIsValid($checksum, $tags);
    }
    $sum = 0;
    foreach ($tags as $tag) {
      if (!isset($this->tagCache[$tag])) {
        return $this->traitIsValid($checksum, $tags);
      }
      $sum += $this->tagCache[$tag];
    }
    return $checksum == $sum;
  }

  public function getTagKey(string $tag): string {
    return $this->prefix . '::' . $tag;
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
  }

  // Only reached on invalidation, for the trait's transaction check.
  protected function getDatabaseConnection() {
    return \Drupal::database();
  }

}
