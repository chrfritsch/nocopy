<?php

declare(strict_types=1);

namespace Drupal\nocopy\Cache;

use Drupal\Component\Datetime\TimeInterface;
use Drupal\Core\Cache\CacheFactoryInterface;
use Drupal\Core\Cache\CacheTagsChecksumInterface;
use Drupal\Core\Site\Settings;

class NocopyBackendFactory implements CacheFactoryInterface {

  protected string $sitePrefix;

  public function __construct(string $root, string $sitePath, protected CacheTagsChecksumInterface $checksumProvider, protected TimeInterface $time) {
    $this->sitePrefix = Settings::getApcuPrefix('nocopy_backend', $root, $sitePath);
  }

  public function get($bin) {
    return new NocopyBackend($bin, $this->sitePrefix, $this->checksumProvider, $this->time);
  }

}
