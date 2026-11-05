<?php

declare(strict_types=1);

namespace Drupal\nocopy\Hook;

use Drupal\Core\Extension\Requirement\RequirementSeverity;
use Drupal\Core\Hook\Attribute\Hook;
use Drupal\Core\StringTranslation\StringTranslationTrait;

class NocopyHooks {

  use StringTranslationTrait;

  #[Hook('runtime_requirements')]
  public function runtimeRequirements(): array {
    $with = function_exists('nocopy_shared_with') ? nocopy_shared_with() : NULL;
    if ($with !== 'host' && $with !== 'valkey') {
      return ['nocopy' => [
        'title' => 'nocopy',
        'value' => $with === NULL ? $this->t('Extension not loaded') : $this->t('No shared cache'),
        'severity' => RequirementSeverity::Warning,
        'description' => $this->t('Set nocopy.shm_name (one server) or nocopy.valkey (several) in php.ini. Without it nocopy caches nothing.'),
      ]];
    }
    $stats = nocopy_stats();
    return ['nocopy' => [
      'title' => 'nocopy',
      // evicted steadily growing: raise nocopy.shm_size.
      'value' => $this->t('Shared with @with, @used of @size MB used, @evicted evicted', [
        '@with' => $with,
        '@used' => round($stats['used'] / 1048576, 1),
        '@size' => round($stats['size'] / 1048576),
        '@evicted' => $stats['evicted'],
      ]),
      'severity' => RequirementSeverity::OK,
    ]];
  }

}
