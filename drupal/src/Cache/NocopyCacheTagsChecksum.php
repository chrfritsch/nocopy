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

  use CacheTagsChecksumTrait;

  protected string $prefix;

  public function __construct(string $root, string $sitePath) {
    $this->prefix = Settings::getApcuPrefix('nocopy_tags', $root, $sitePath);
  }

  public function getTagKey(string $tag): string {
    return $this->prefix . '::' . $tag;
  }

  protected function getTagInvalidationCounts(array $tags) {
    $counts = [];
    foreach ($tags as $tag) {
      $token = nocopy_get($this->getTagKey($tag), $hit);
      if (!$hit) {
        $token = random_int(1, 2 ** 48);
        nocopy_put($this->getTagKey($tag), $token);
      }
      $counts[$tag] = $token;
    }
    return $counts;
  }

  protected function doInvalidateTags(array $tags) {
    foreach ($tags as $tag) {
      nocopy_put($this->getTagKey($tag), random_int(1, 2 ** 48));
    }
  }

  // Only reached on invalidation, for the trait's transaction check.
  protected function getDatabaseConnection() {
    return \Drupal::database();
  }

}
