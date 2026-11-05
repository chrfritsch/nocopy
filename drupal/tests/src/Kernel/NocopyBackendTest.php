<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\Core\Cache\CacheBackendInterface;
use Drupal\Core\DependencyInjection\ContainerBuilder;
use Drupal\Core\Render\Markup;
use Drupal\KernelTests\Core\Cache\GenericCacheBackendUnitTestBase;
use Drupal\nocopy\Cache\NocopyBackend;
use Drupal\nocopy\Cache\NocopyCacheTagsChecksum;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\IgnoreDeprecations;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * Core's generic cache backend suite against NocopyBackend.
 */
#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopyBackendTest extends GenericCacheBackendUnitTestBase {

  protected string $prefix;

  // The generic suite (tags, $allow_invalid) then runs against nocopy tag tokens.
  public function register(ContainerBuilder $container) {
    parent::register($container);
    $container->register('cache_tags.invalidator.checksum', NocopyCacheTagsChecksum::class)
      ->setArguments(['%app.root%', '%site.path%'])
      ->addTag('cache_tags_invalidator');
  }

  protected function createCacheBackend($bin) {
    $this->prefix ??= 'test' . $this->randomMachineName();
    return new NocopyBackend($bin, $this->prefix, \Drupal::service('cache_tags.invalidator.checksum'), \Drupal::service('datetime.time'));
  }

  public function testGenericSuiteUsesNocopyTags(): void {
    $this->assertInstanceOf(NocopyCacheTagsChecksum::class, \Drupal::service('cache_tags.invalidator.checksum'));
  }

  public function testObjectsRoundTrip(): void {
    $b = $this->getCacheBackend();
    $data = ['#markup' => Markup::create('<b>x</b>'), 'o' => (object) ['a' => [1]], 'arr' => [1, 2]];
    $b->set('obj', $data);
    $item = $b->get('obj');
    $this->assertInstanceOf(Markup::class, $item->data['#markup']);
    $this->assertSame('<b>x</b>', (string) $item->data['#markup']);
    $this->assertSame([1], $item->data['o']->a);
  }

  public function testDeleteAllIsolatedAndSeenByNewInstance(): void {
    $a = $this->getCacheBackend('bin_a');
    $b = $this->getCacheBackend('bin_b');
    $a->set('k', 1);
    $b->set('k', 2);
    $a->deleteAll();
    $this->assertFalse($this->createCacheBackend('bin_a')->get('k'));
    $this->assertSame(2, $b->get('k')->data);
  }

  public function testReturnsPlainArrayDataUnchanged(): void {
    $b = $this->getCacheBackend();
    $big = range(1, 1000);
    $b->set('big', $big);
    $this->assertSame($big, $b->get('big')->data);
  }

  public function testEvictionIsAMiss(): void {
    $b = $this->getCacheBackend();
    $value = fn(int $i) => array_fill(0, 200, "v$i");
    for ($i = 0; $i < 3000; $i++) {
      $b->set("e$i", $value($i));
    }
    $hits = 0;
    for ($i = 0; $i < 3000; $i++) {
      $item = $b->get("e$i");
      if ($item !== FALSE) {
        $this->assertSame($value($i), $item->data);
        $hits++;
      }
    }
    $this->assertGreaterThan(0, $hits);
  }

  public function testDeletedItemsStayDeletedWhenVersionKeyIsLost(): void {
    $b = $this->createCacheBackend('fresh_bin');
    $b->set('k', 'old');
    $b->deleteAll();
    nocopy_delete($this->prefix . '::fresh_bin::version');
    $this->assertFalse($b->get('k'));
  }

  public function testPermanentTtlOnlyWithValkey(): void {
    $ttl = fn (string $bin) => (fn ($expire) => $this->ttl($expire))->call($this->createCacheBackend($bin), CacheBackendInterface::CACHE_PERMANENT);
    $this->assertSame(nocopy_shared_with() === 'valkey' ? 31536000 : 0, $ttl('render'));
    $this->assertSame(0, $ttl('config'));
  }

  public function testDeleteInsideTransactionWaitsForCommit(): void {
    $b = $this->getCacheBackend();
    $b->set('k', 'v');
    $b->set('kept', 'v');
    $tx = \Drupal::database()->startTransaction();
    $b->delete('k');
    $this->assertFalse($b->get('k'));
    // Another request does not see the delete before the commit.
    $this->assertSame('v', $this->createCacheBackend($this->getTestBin())->get('k')->data);
    unset($tx);
    $this->assertFalse($this->createCacheBackend($this->getTestBin())->get('k'));

    $tx = \Drupal::database()->startTransaction();
    $b->delete('kept');
    $tx->rollBack();
    unset($tx);
    $this->assertSame('v', $b->get('kept')->data);
  }

  #[IgnoreDeprecations]
  public function testInvalidatedItemsStayInvalidWhenMarkerIsLost(): void {
    $b = $this->getCacheBackend();
    $b->set('k', 'v');
    $this->expectUserDeprecationMessage('CacheBackendInterface::invalidateAll() is deprecated in drupal:11.2.0 and is removed from drupal:12.0.0. Use CacheBackendInterface::deleteAll() or cache tag invalidation instead. See https://www.drupal.org/node/3500622');
    $b->invalidateAll();
    nocopy_delete($this->prefix . '::' . $this->getTestBin() . '::invalidated_at');
    $this->assertFalse($b->get('k'));
  }

}
