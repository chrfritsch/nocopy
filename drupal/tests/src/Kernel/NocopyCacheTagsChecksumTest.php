<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\KernelTests\KernelTestBase;
use Drupal\nocopy\Cache\NocopyCacheTagsChecksum;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopyCacheTagsChecksumTest extends KernelTestBase {

  protected NocopyCacheTagsChecksum $checksum;

  protected function setUp(): void {
    parent::setUp();
    $this->checksum = new NocopyCacheTagsChecksum($this->root, 'sites/test' . $this->randomMachineName());
  }

  public function testInvalidationChangesChecksum(): void {
    $c = $this->checksum->getCurrentChecksum(['a', 'b']);
    $this->checksum->invalidateTags(['a']);
    $this->checksum->reset();
    $this->assertFalse($this->checksum->isValid($c, ['a', 'b']));
  }

  public function testUnrelatedInvalidationKeepsValid(): void {
    $c = $this->checksum->getCurrentChecksum(['a']);
    $this->checksum->invalidateTags(['z']);
    $this->checksum->reset();
    $this->assertTrue($this->checksum->isValid($c, ['a']));
  }

  public function testEvictedTokenInvalidates(): void {
    $c = $this->checksum->getCurrentChecksum(['a']);
    nocopy_delete($this->checksum->getTagKey('a'));
    $this->checksum->reset();
    $this->assertFalse($this->checksum->isValid($c, ['a']));
  }

  public function testTokensAreFreshAndInRange(): void {
    $seen = [];
    for ($i = 0; $i < 1000; $i++) {
      $this->checksum->invalidateTags(['t']);
      $this->checksum->reset();
      $t = nocopy_get($this->checksum->getTagKey('t'));
      $this->assertIsInt($t);
      $this->assertGreaterThanOrEqual(1, $t);
      $this->assertLessThanOrEqual(2 ** 48, $t);
      $seen[$t] = TRUE;
    }
    $this->assertCount(1000, $seen);
  }

}
