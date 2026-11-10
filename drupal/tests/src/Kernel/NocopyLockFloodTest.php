<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\KernelTests\KernelTestBase;
use Drupal\nocopy\Flood\NocopyFlood;
use Drupal\nocopy\Lock\NocopyLockBackend;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * Two backends with their own lock ids stand for two processes.
 */
#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopyLockFloodTest extends KernelTestBase {

  protected string $site;

  protected function setUp(): void {
    parent::setUp();
    $this->site = 'sites/test' . $this->randomMachineName();
  }

  protected function lock(bool $persistent = FALSE): NocopyLockBackend {
    return new NocopyLockBackend($this->root, $this->site, $persistent);
  }

  public function testExclusive(): void {
    [$a, $b] = [$this->lock(), $this->lock()];
    $this->assertTrue($a->lockMayBeAvailable('x'));
    $this->assertTrue($a->acquire('x'));
    $this->assertFalse($b->acquire('x'));
    $this->assertFalse($b->lockMayBeAvailable('x'));
    $b->release('x');
    $this->assertFalse($b->acquire('x'), 'Releasing a lock held by another process does nothing.');
    $this->assertTrue($a->acquire('x'), 'The holder extends it.');
    $a->release('x');
    $this->assertTrue($b->lockMayBeAvailable('x'));
    $this->assertTrue($b->acquire('x'));
    $this->assertFalse($a->acquire('x'));
  }

  public function testExpiredLockIsTakenOver(): void {
    [$a, $b] = [$this->lock(), $this->lock()];
    $this->assertTrue($a->acquire('x', 0.1));
    usleep(300000);
    $this->assertTrue($b->lockMayBeAvailable('x'));
    $this->assertTrue($b->acquire('x'));
    $this->assertFalse($a->acquire('x'), 'A lock lost to another process is not extended.');
    $a->release('x');
    $this->assertFalse($a->acquire('x'), 'Nor released by its former holder.');
  }

  public function testReleaseAll(): void {
    [$a, $b] = [$this->lock(), $this->lock()];
    $this->assertTrue($a->acquire('x'));
    $this->assertTrue($a->acquire('y'));
    $a->releaseAll();
    $this->assertTrue($b->acquire('x'));
    $this->assertTrue($b->acquire('y'));
  }

  public function testPersistentLockSpansProcesses(): void {
    [$p1, $p2, $a] = [$this->lock(TRUE), $this->lock(TRUE), $this->lock()];
    $this->assertSame('persistent', $p1->getLockId());
    $this->assertTrue($p1->acquire('x'));
    $this->assertFalse($a->acquire('x'));
    $p2->release('x');
    $this->assertTrue($a->acquire('x'));
  }

  public function testFlood(): void {
    $flood = new NocopyFlood(\Drupal::requestStack(), $this->root, $this->site);
    for ($i = 0; $i < 3; $i++) {
      $this->assertTrue($flood->isAllowed('login', 3));
      $flood->register('login', 1);
    }
    $this->assertFalse($flood->isAllowed('login', 3));
    $this->assertTrue($flood->isAllowed('login', 4));
    $this->assertTrue($flood->isAllowed('login', 3, 3600, 'other'));
    $this->assertTrue($flood->isAllowed('contact', 3));
    $flood->register('login', 3600, 'other');
    $flood->clear('login', 'other');
    $this->assertTrue($flood->isAllowed('login', 1, 3600, 'other'));
    sleep(2);
    $this->assertTrue($flood->isAllowed('login', 3), 'Events expire after the window.');
  }

}
