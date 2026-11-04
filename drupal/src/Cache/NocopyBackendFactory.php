<?php

declare(strict_types=1);

namespace Drupal\nocopy\Cache;

use Drupal\Component\Datetime\TimeInterface;
use Drupal\Core\Cache\CacheFactoryInterface;
use Drupal\Core\Cache\CacheTagsChecksumInterface;
use Drupal\Core\Cache\NullBackend;
use Drupal\Core\Site\Settings;

class NocopyBackendFactory implements CacheFactoryInterface {

  protected string $sitePrefix;

  public function __construct(string $root, string $sitePath, protected CacheTagsChecksumInterface $checksumProvider, protected TimeInterface $time, protected bool $fastLayer = FALSE) {
    $this->sitePrefix = Settings::getApcuPrefix('nocopy_backend', $root, $sitePath);
  }

  public function get($bin) {
    if (!$this->fastLayer) {
      return new NocopyBackend($bin, $this->sitePrefix, $this->checksumProvider, $this->time);
    }
    // Like core's APCu fast layer: without the extension (e.g. Drush) chainedfast just uses the database.
    return function_exists('nocopy_get') ? new NocopyBackend($bin, $this->sitePrefix, $this->checksumProvider, $this->time, NULL) : new NullBackend($bin);
  }

}
