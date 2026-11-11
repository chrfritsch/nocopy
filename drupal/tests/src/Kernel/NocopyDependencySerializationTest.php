<?php

declare(strict_types=1);

namespace Drupal\Tests\nocopy\Kernel;

use Drupal\Core\DependencyInjection\DependencySerializationTrait;
use Drupal\Core\Entity\EntityStorageInterface;
use Drupal\Core\Language\LanguageManagerInterface;
use Drupal\KernelTests\KernelTestBase;
use Drupal\node\Entity\Node;
use Drupal\nocopy\Cache\NocopyBackend;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RequiresPhpExtension;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * DependencySerializationTrait through the cache: services come back as the container's live ones, like unserialize().
 */
#[Group('nocopy')]
#[RequiresPhpExtension('nocopy')]
#[RunTestsInSeparateProcesses]
class NocopyDependencySerializationTest extends KernelTestBase {

  protected static $modules = ['nocopy', 'system', 'user', 'node', 'field', 'text', 'filter'];

  protected function setUp(): void {
    parent::setUp();
    if (!in_array(nocopy_shared_with(), ['host', 'valkey'], TRUE)) {
      $this->markTestSkipped('needs a shared cache');
    }
    $this->installEntitySchema('user');
    $this->installEntitySchema('node');
  }

  protected function backend(): NocopyBackend {
    return new NocopyBackend('dst', 'dst' . $this->randomMachineName(), \Drupal::service('cache_tags.invalidator.checksum'), \Drupal::service('datetime.time'));
  }

  public function testNodeRoundTrip(): void {
    $node = Node::create(['type' => 'page', 'title' => 'Hello', 'langcode' => 'en']);
    $b = $this->backend();
    $b->set('n', $node);
    $expected = unserialize(serialize($node));
    for ($i = 0; $i < 3; $i++) {
      $got = $b->get('n')->data;
      $this->assertInstanceOf(Node::class, $got);
      $this->assertNotSame($node, $got);
      $this->assertSame('Hello', $got->getTitle());
      $this->assertSame('en', $got->language()->getId());
      $this->assertSame($expected->toArray(), $got->toArray());
      $this->assertSame(serialize($expected), serialize($got));
    }
    $got->setTitle('changed');
    $this->assertSame('Hello', $b->get('n')->data->getTitle());
  }

  public function testServicesAreTheLiveOnes(): void {
    $o = new DependencySerializationHolder(\Drupal::languageManager(), \Drupal::entityTypeManager()->getStorage('node'));
    $b = $this->backend();
    $b->set('h', $o);
    $got = $b->get('h')->data;
    $this->assertNotSame($o, $got);
    $this->assertSame(\Drupal::languageManager(), $got->languageManager);
    $this->assertSame(\Drupal::entityTypeManager()->getStorage('node'), $got->storage);
    $this->assertSame(['a' => [1, 2]], $got->plain);

    // Only the service ID was cached: a replaced service is what the next fetch gets.
    $new = clone \Drupal::languageManager();
    $this->container->set('language_manager', $new);
    $this->assertSame($new, $b->get('h')->data->languageManager);
  }

}

/**
 * A class holding services through the trait.
 */
class DependencySerializationHolder {

  use DependencySerializationTrait;

  public array $plain = ['a' => [1, 2]];

  public function __construct(public LanguageManagerInterface $languageManager, public EntityStorageInterface $storage) {}

}
