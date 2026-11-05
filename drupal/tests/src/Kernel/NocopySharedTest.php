<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\Core\Database\Database;
use Drupal\KernelTests\KernelTestBase;
use Drupal\nocopy\Cache\NocopyBackend;
use Drupal\nocopy\Cache\NocopyCacheTagsChecksum;
use Drupal\nocopy\Cache\NocopyJournal;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * $settings['nocopy_shared']: no journal where every process shares the cache, an exception where this one does not.
 */
#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopySharedTest extends KernelTestBase {

  protected static $modules = ['nocopy'];

  protected function setUp(): void {
    parent::setUp();
    $this->installSchema('nocopy', ['nocopy_journal']);
    $this->setSetting('nocopy_shared', TRUE);
  }

  protected ?NocopyCacheTagsChecksum $checksum = NULL;

  // A CLI process's bin: a journal writer unless the cache is shared.
  protected function cliBin(): NocopyBackend {
    $site = $this->randomMachineName();
    $this->checksum = new NocopyCacheTagsChecksum($this->root, "sites/$site", new NocopyJournal(TRUE));
    return new NocopyBackend('b', $site, $this->checksum, \Drupal::service('datetime.time'), new NocopyJournal(TRUE));
  }

  public function testSharedCacheWritesNoJournal(): void {
    if (!in_array(nocopy_shared_with(), ['host', 'valkey'], TRUE)) {
      $this->expectException(\RuntimeException::class);
      $this->expectExceptionMessage("nocopy: \$settings['nocopy_shared'] is set, but this cli process does not share the cache (nocopy_shared_with(): " . nocopy_shared_with() . ')');
    }
    $b = $this->cliBin();
    $b->set('k', 'v', tags: ['t']);
    $b->delete('k');
    $b->deleteAll();
    $this->checksum->invalidateTags(['t']);
    $this->assertSame(0, (int) Database::getConnection()->query('SELECT COUNT(*) FROM {nocopy_journal}')->fetchField());
  }

  public function testUnsetSettingKeepsTheJournal(): void {
    $this->setSetting('nocopy_shared', FALSE);
    $this->cliBin()->set('k', 'v');
    $this->assertSame(1, (int) Database::getConnection()->query('SELECT COUNT(*) FROM {nocopy_journal}')->fetchField());
  }

}
