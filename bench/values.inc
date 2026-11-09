<?php
// Drupal-like values for compress.php and block.php: $values
mt_srand(42);
$word = fn() => substr(str_shuffle('abcdefghijklmnopqrstuvwxyz'), 0, mt_rand(3, 10));
$text = fn($n) => implode(' ', array_map(fn() => $word(), range(1, $n)));

$rows = [];
for ($i = 0; $i < 1000; $i++) {
    $rows[] = ['nid' => $i, 'type' => ['article', 'page'][$i % 2], 'title' => $text(6), 'status' => 1, 'created' => 1700000000 + $i, 'uid' => $i % 17];
}
$render = function ($d) use (&$render, $text) {
    $a = ['#theme' => 'item', '#markup' => '<div class="field field--name-body">' . $text(20) . '</div>',
          '#cache' => ['tags' => ['node:' . mt_rand(1, 999), 'config:system.site'], 'contexts' => ['languages:language_interface', 'theme', 'user.permissions'], 'max-age' => -1],
          '#attached' => ['library' => ['core/drupal', 'system/base']]];
    if ($d) for ($i = 0; $i < 4; $i++) $a["child$i"] = $render($d - 1);
    return $a;
};
$config = [];
for ($i = 0; $i < 200; $i++) $config["module.setting_$i"] = ['label' => $text(3), 'enabled' => (bool) ($i % 3), 'weight' => $i];
$entity = new stdClass();
foreach (range(1, 40) as $i) $entity->{"field_$i"} = [['value' => $text(8), 'format' => 'basic_html']];
$html = '';
for ($i = 0; $i < 300; $i++) $html .= '<li class="menu-item"><a href="/node/' . mt_rand(1, 9999) . '">' . $text(4) . "</a></li>\n";

$values = [
    'small_array' => ['id' => 7, 'name' => 'x', 'roles' => ['a', 'b'], 'ok' => true, 'ts' => 1700000000],
    'config_200' => $config,
    'render_d3' => $render(3),
    'entity_obj' => $entity,
    'db_rows_1k' => $rows,
    'html_20k' => $html,
    'random_4k' => random_bytes(4096),
];

function ns(callable $f, $arg): float
{
    $n = 1;
    do {
        $t = hrtime(true);
        for ($i = 0; $i < $n; $i++) $f($arg);
        $d = hrtime(true) - $t;
        $n *= 2;
    } while ($d < 50e6);
    $best = INF;
    for ($r = 0; $r < 5; $r++) {
        $t = hrtime(true);
        for ($i = 0; $i < $n; $i++) $f($arg);
        $best = min($best, (hrtime(true) - $t) / $n);
    }
    return $best;
}
