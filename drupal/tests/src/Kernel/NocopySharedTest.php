<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\KernelTests\KernelTestBase;
use Drupal\nocopy\Cache\NocopyBackend;
use Drupal\nocopy\Cache\NocopyBackendFactory;
use Drupal\nocopy\Cache\NocopyCacheTagsChecksum;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * The backend and tag checksums need a cache shared by every process of the site; the fast layer does not.
 */
#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopySharedTest extends KernelTestBase {

  protected static $modules = ['nocopy'];

  protected function shared(): bool {
    return in_array(nocopy_shared_with(), ['host', 'valkey'], TRUE);
  }

  public function testBackendNeedsSharedCache(): void {
    if (!$this->shared()) {
      $this->expectException(\RuntimeException::class);
      $this->expectExceptionMessage('nocopy: this cli process does not share the cache with the rest of the site (nocopy_shared_with(): ' . nocopy_shared_with() . ')');
    }
    $b = new NocopyBackend('b', 'p', \Drupal::service('cache_tags.invalidator.checksum'), \Drupal::service('datetime.time'));
    $b->set('k', 'v');
    $this->assertSame('v', $b->get('k')->data);
  }

  public function testChecksumNeedsSharedCache(): void {
    if (!$this->shared()) {
      $this->expectException(\RuntimeException::class);
    }
    $this->assertInstanceOf(NocopyCacheTagsChecksum::class, new NocopyCacheTagsChecksum($this->root, 'sites/x'));
  }

  public function testFastLayerWorksEverywhere(): void {
    $factory = new NocopyBackendFactory($this->root, 'sites/x', \Drupal::service('cache_tags.invalidator.checksum'), \Drupal::service('datetime.time'), TRUE);
    $b = $factory->get('fast');
    $b->set('k', 'v');
    $this->assertSame('v', $b->get('k')->data);
  }

}
