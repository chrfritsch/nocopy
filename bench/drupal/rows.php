<?php
// drush php:script, with nocopy loaded: every row of the database cache bins, put one by one into the arena; prints rows, bytes.
$db = \Drupal::database();
$u = nocopy_stats()['used'];
$rows = $failed = 0;
foreach ($db->query("SHOW TABLES LIKE 'cache%'")->fetchCol() as $t) {
    if ($t === 'cachetags') {
        continue;
    }
    foreach ($db->query("SELECT cid, data, serialized FROM {$t}") as $r) {
        $ok = nocopy_put("$t:$r->cid", $r->serialized ? unserialize($r->data) : $r->data);
        $rows++;
        $failed += !$ok;
    }
}
$s = nocopy_stats();
printf("rows\t%d\tfailed\t%d\tarena_bytes\t%d\tshared_strings\t%d\tshared_bytes\t%d\n", $rows, $failed, $s['used'] - $u, $s['shared_strings'] ?? 0, $s['shared_strings_used'] ?? 0);
