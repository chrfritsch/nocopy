<?php

declare(strict_types=1);

namespace Drupal\nocopy\Cache;

use Drupal\Core\Database\Database;
use Drupal\Core\Database\StatementInterface;
use Drupal\Core\Site\Settings;

/**
 * CLI processes (Drush, cron) have their own nocopy memory: they log cache writes here, web requests replay them.
 *
 * Not with $settings['nocopy_shared']: every process then uses one cache (nocopy.shm_name or nocopy.valkey), which carries
 * every write itself. settings.php is read by php-fpm and Drush alike, so they agree on it; their php.ini may differ.
 */
final class NocopyJournal {

  public const TABLE = 'nocopy_journal', TAGS = ':tags', PRUNED = ':pruned';

  // [request, last id]: one query per web request, shared by all bins.
  private static array $last = [NULL, 0];

  private static bool $tableExists = FALSE;

  public function __construct(public readonly bool $writer = PHP_SAPI === 'cli') {}

  // The site's choice, checked in each process: a process with a cache of its own would write where no one else reads.
  public static function shared(): bool {
    if (!Settings::get('nocopy_shared', FALSE)) {
      return FALSE;
    }
    $with = nocopy_shared_with();
    if ($with !== 'host' && $with !== 'valkey') {
      throw new \RuntimeException(sprintf("nocopy: \$settings['nocopy_shared'] is set, but this %s process does not share the cache (nocopy_shared_with(): %s). Set nocopy.shm_name (or nocopy.valkey) in the php.ini of every PHP that runs this site, and run Drush and cron as the user that starts php-fpm.", PHP_SAPI, $with));
    }
    return TRUE;
  }

  public static function request(): float {
    return (float) ($_SERVER['REQUEST_TIME_FLOAT'] ?? 0);
  }

  // NULL: the whole bin.
  public function record(string $name, ?array $items): void {
    if (!$this->writer || self::shared()) {
      return;
    }
    $db = Database::getConnection();
    // No table before the module is installed.
    if (!self::$tableExists && !(self::$tableExists = $db->schema()->tableExists(self::TABLE))) {
      return;
    }
    $db->insert(self::TABLE)->fields([
      'created' => time(),
      'name' => $name,
      'items' => $items === NULL ? NULL : json_encode(array_map('strval', array_values($items))),
    ])->execute();
  }

  public function lastId(): int {
    if (self::$last[0] !== self::request()) {
      self::$last = [self::request(), (int) $this->query('SELECT MAX([id]) FROM {nocopy_journal}')?->fetchField()];
    }
    return self::$last[1];
  }

  // Entries for $name after $applied, oldest first; [NULL] when pruning removed some of them.
  public function since(int $applied, string $name): array {
    $entries = [];
    $rows = $this->query('SELECT [name], [items] FROM {nocopy_journal} WHERE [id] > :id AND [name] IN (:names[]) ORDER BY [id]', [':id' => $applied, ':names[]' => [$name, self::PRUNED]]) ?? [];
    foreach ($rows as $row) {
      $items = $row->items === NULL ? NULL : json_decode($row->items);
      if ($row->name !== self::PRUNED) {
        $entries[] = $items;
      }
      elseif ($items[0] > $applied) {
        return [NULL];
      }
    }
    return $entries;
  }

  private function query(string $sql, array $args = []): ?StatementInterface {
    try {
      return Database::getConnection()->query($sql, $args);
    }
    // No table or no database yet: nothing to replay.
    catch (\Exception) {
      return NULL;
    }
  }

  // Drops entries older than $maxAge; a marker row makes readers that had not seen them start over.
  public static function prune(int $maxAge = 86400): void {
    $db = Database::getConnection();
    $max = (int) $db->query('SELECT MAX([id]) FROM {nocopy_journal} WHERE [created] < :t AND [name] <> :p', [':t' => time() - $maxAge, ':p' => self::PRUNED])->fetchField();
    if ($max) {
      $db->delete(self::TABLE)->condition('id', $max, '<=')->execute();
      $db->insert(self::TABLE)->fields(['created' => time(), 'name' => self::PRUNED, 'items' => json_encode([$max])])->execute();
    }
  }

}
