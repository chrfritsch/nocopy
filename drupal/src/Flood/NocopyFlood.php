<?php

declare(strict_types=1);

namespace Drupal\nocopy\Flood;

use Drupal\Core\Flood\FloodInterface;
use Drupal\Core\Site\Settings;
use Drupal\nocopy\Cache\NocopyBackend;
use Symfony\Component\HttpFoundation\RequestStack;

/**
 * Flood control in nocopy: one counter per event and identifier, counted with nocopy_inc() (atomic, also in Valkey).
 */
class NocopyFlood implements FloodInterface {

  protected string $prefix;

  public function __construct(protected RequestStack $requestStack, string $root, string $sitePath) {
    NocopyBackend::requireShared();
    $this->prefix = (Settings::get('nocopy_prefix') ?: hash('xxh128', $root . '/' . $sitePath)) . '.flood::';
  }

  // ponytail: a fixed window from the first event (as Laravel's limiter), so up to 2x $threshold across its edge; time buckets if that matters.
  public function register($name, $window = 3600, $identifier = NULL) {
    nocopy_inc($this->key($name, $identifier), 1, max(1, (int) $window));
  }

  public function clear($name, $identifier = NULL) {
    nocopy_delete($this->key($name, $identifier));
  }

  public function isAllowed($name, $threshold, $window = 3600, $identifier = NULL) {
    return (nocopy_get($this->key($name, $identifier)) ?? 0) < $threshold;
  }

  // Counters expire by themselves.
  public function garbageCollection() {}

  protected function key(string $name, $identifier): string {
    return $this->prefix . $name . '::' . ($identifier ?? $this->requestStack->getCurrentRequest()?->getClientIp());
  }

}
