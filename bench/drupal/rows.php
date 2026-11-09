<?php
// drush php:script, with nocopy loaded: every row of the database cache bins, put one by one into the arena and read back; prints rows, bytes.
$db = \Drupal::database();
$u = nocopy_stats()['used'];
$rows = $failed = $differ = 0;
foreach ($db->query("SHOW TABLES LIKE 'cache%'")->fetchCol() as $t) {
    if ($t === 'cachetags') {
        continue;
    }
    foreach ($db->query("SELECT cid, data, serialized FROM {$t}") as $r) {
        $v = $r->serialized ? unserialize($r->data) : $r->data;
        $ok = nocopy_put("$t:$r->cid", $v);
        $rows++;
        $failed += !$ok;
        $differ += $ok && serialize(nocopy_get("$t:$r->cid")) !== serialize($v);
    }
}
$s = nocopy_stats();
printf("rows\t%d\tfailed\t%d\tdiffer\t%d\tarena_bytes\t%d\tshared_strings\t%d\tshared_bytes\t%d\n", $rows, $failed, $differ, $s['used'] - $u, $s['shared_strings'] ?? 0, $s['shared_strings_used'] ?? 0);
