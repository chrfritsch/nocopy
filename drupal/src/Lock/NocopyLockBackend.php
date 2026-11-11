<?php

declare(strict_types=1);

namespace Drupal\nocopy\Lock;

use Drupal\Core\Lock\LockBackendAbstract;
use Drupal\nocopy\Cache\NocopyBackend;

/**
 * Locks in nocopy: one integer per lock, its expiry and owner, changed only by nocopy_add() and nocopy_cas() (atomic, also in Valkey).
 *
 * Expiry in the value, not as a TTL, so extending and taking over an expired lock are one compare-and-swap. Released: 0, never
 * deleted (a delete could remove the next owner's lock). No TTL, so Valkey's volatile-* eviction never drops a held lock;
 * a full arena without Valkey can, like any cache.
 */
class NocopyLockBackend extends LockBackendAbstract {

  // Value: deciseconds since 2025 (34 bits, until 2079) << 29 | owner (29 bits of crc32(lock id)).
  private const EPOCH = 1735689600, OWNER_BITS = 29, OWNER_MASK = (1 << 29) - 1;

  protected string $prefix;

  public function __construct(string $root, string $sitePath, bool $persistent = FALSE) {
    NocopyBackend::requireShared();
    $this->prefix = NocopyBackend::sitePrefix($root, $sitePath) . '.lock::';
    if ($persistent) {
      // Like core's persistent backend: one lock id across requests, nothing released at shutdown.
      $this->lockId = 'persistent';
    }
    else {
      drupal_register_shutdown_function([$this, 'releaseAll']);
    }
  }

  public function acquire($name, $timeout = 30.0) {
    $key = $this->prefix . $name;
    $new = (int) ceil((microtime(TRUE) - self::EPOCH + max($timeout, 0.001)) * 10) << self::OWNER_BITS | $this->owner();
    // Held: extend. Else free, released or expired: take it.
    $old = $this->locks[$name] ?? nocopy_get($key);
    $ok = $old === NULL ? nocopy_add($key, $new) : is_int($old) && (isset($this->locks[$name]) || self::expired($old)) && nocopy_cas($key, $old, $new);
    if ($ok) {
      $this->locks[$name] = $new;
    }
    else {
      unset($this->locks[$name]);
    }
    return $ok;
  }

  public function lockMayBeAvailable($name) {
    $v = nocopy_get($this->prefix . $name);
    return !is_int($v) || self::expired($v);
  }

  public function release($name) {
    // Our own value first: a copy read from Valkey may not show our last write yet.
    $v = $this->locks[$name] ?? nocopy_get($this->prefix . $name);
    unset($this->locks[$name]);
    if (is_int($v) && $v !== 0 && ($v & self::OWNER_MASK) === $this->owner()) {
      nocopy_cas($this->prefix . $name, $v, 0);
    }
  }

  public function releaseAll($lockId = NULL) {
    foreach (array_keys($this->locks) as $name) {
      $this->release((string) $name);
    }
  }

  private function owner(): int {
    return crc32($this->getLockId()) & self::OWNER_MASK;
  }

  private static function expired(int $v): bool {
    return ($v >> self::OWNER_BITS) <= (microtime(TRUE) - self::EPOCH) * 10;
  }

}
