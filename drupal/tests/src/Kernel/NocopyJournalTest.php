<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\Core\Cache\CacheBackendInterface;
use Drupal\Core\Database\Database;
use Drupal\KernelTests\KernelTestBase;
use Drupal\nocopy\Cache\NocopyBackend;
use Drupal\nocopy\Cache\NocopyCacheTagsChecksum;
use Drupal\nocopy\Cache\NocopyJournal;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * CLI cache writes reach web requests. CLI and web get separate prefixes, like their separate nocopy memories.
 */
#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopyJournalTest extends KernelTestBase {

  protected static $modules = ['nocopy'];

  protected string $site;
  protected array $checksums = [];

  protected function setUp(): void {
    parent::setUp();
    $this->installSchema('nocopy', ['nocopy_journal']);
    $this->site = $this->randomMachineName();
  }

  protected function checksum(bool $cli): NocopyCacheTagsChecksum {
    return $this->checksums[(int) $cli] ??= new NocopyCacheTagsChecksum($this->root, "sites/$this->site-$cli", new NocopyJournal($cli));
  }

  protected function bin(string $bin, bool $cli): NocopyBackend {
    return new NocopyBackend($bin, "$this->site-$cli", $this->checksum($cli), \Drupal::service('datetime.time'), new NocopyJournal($cli));
  }

  protected function nextRequest(): void {
    $_SERVER['REQUEST_TIME_FLOAT'] = ($_SERVER['REQUEST_TIME_FLOAT'] ?? 0) + 1;
  }

  public function testCliSetDeletesTheWebItem(): void {
    $web = $this->bin('b', FALSE);
    $web->set('k', 'old');
    $web->set('other', 'x');
    $this->bin('b', TRUE)->set('k', 'new');
    $this->nextRequest();
    $this->assertFalse($web->get('k'));
    $this->assertSame('x', $web->get('other')->data);
  }

  public function testCliDeleteAllEmptiesOnlyThatWebBin(): void {
    $web = $this->bin('b', FALSE);
    $webC = $this->bin('c', FALSE);
    $web->set('k', 1);
    $webC->set('k', 2);
    $this->bin('b', TRUE)->deleteAll();
    $this->nextRequest();
    $this->assertFalse($web->get('k'));
    $this->assertSame(2, $webC->get('k')->data);
  }

  public function testCliTagInvalidationReachesWeb(): void {
    $web = $this->bin('b', FALSE);
    $web->set('a', 1, CacheBackendInterface::CACHE_PERMANENT, ['node:1']);
    $web->set('z', 2, CacheBackendInterface::CACHE_PERMANENT, ['node:2']);
    $this->checksum(TRUE)->invalidateTags(['node:1']);
    $this->nextRequest();
    $this->assertFalse($web->get('a'));
    $this->assertSame(2, $web->get('z')->data);
  }

  public function testOneJournalQueryPerRequest(): void {
    $bins = [$this->bin('b', FALSE), $this->bin('c', FALSE), $this->bin('d', FALSE)];
    foreach ($bins as $bin) {
      $bin->set('k', 1, CacheBackendInterface::CACHE_PERMANENT, ['t']);
    }
    $this->nextRequest();
    Database::startLog('journal');
    foreach ($bins as $bin) {
      $this->assertSame(1, $bin->get('k')->data);
    }
    $queries = array_filter(Database::getLog('journal'), fn($q) => str_contains($q['query'], 'nocopy_journal'));
    $this->assertCount(1, $queries);
  }

  public function testWebStartsOverWhenItsPositionIsLost(): void {
    $web = $this->bin('b', FALSE);
    $web->set('k', 1);
    nocopy_delete("$this->site-::b::applied");
    $this->nextRequest();
    $this->assertFalse($web->get('k'));
  }

  public function testPrunedJournalStartsOverOnlyBehindIt(): void {
    $behind = $this->bin('b', FALSE);
    $behind->set('k', 1);
    $this->bin('other', TRUE)->set('x', 1);
    $this->nextRequest();
    $current = $this->bin('c', FALSE);
    $current->set('k', 2);
    Database::getConnection()->update('nocopy_journal')->fields(['created' => 0])->execute();
    nocopy_cron();
    $this->nextRequest();
    $this->assertFalse($behind->get('k'));
    $this->assertSame(2, $current->get('k')->data);
  }

  public function testWorksWithoutJournalTable(): void {
    Database::getConnection()->schema()->dropTable('nocopy_journal');
    $cli = $this->bin('b', TRUE);
    $cli->set('k', 'v');
    $this->assertSame('v', $cli->get('k')->data);
    $web = $this->bin('b', FALSE);
    $web->set('k', 'w');
    $this->assertSame('w', $web->get('k')->data);
  }

}
