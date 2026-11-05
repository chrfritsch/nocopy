<?php
namespace NocopyBench;

use Drupal\Component\Serialization\SerializationInterface;

// igbinary for drupal/redis, so the redis baselines use the same serializer as nocopy.
class IgbinarySerialize implements SerializationInterface {
  public static function encode($data) { return igbinary_serialize($data); }
  public static function decode($raw) { return igbinary_unserialize($raw); }
  public static function getFileExtension() { return 'igbinary'; }
}
