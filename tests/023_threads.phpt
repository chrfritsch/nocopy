--TEST--
nocopy (ZTS): threads share the arena; each claims its own worker slot and gives it back when it ends
--EXTENSIONS--
nocopy
parallel
--INI--
nocopy.shm_name=/nocopy-t023
apc.shm_size=64M
nocopy.request_scope=0
--FILE--
<?php
nocopy_put('shared', ['a' => range(1, 100)]);
$before = nocopy_stats()['pinned_segments'];
$work = function (int $t) {
    mt_srand($t);
    $model = []; $bad = 0;
    for ($i = 0; $i < 20000; $i++) {
        $k = "t{$t}_" . mt_rand(0, 200);
        $r = mt_rand(0, 9);
        if ($r < 4) {
            $v = array_fill(0, [1, 10, 100, 300][mt_rand(0, 3)], "v$t.$i");
            $e = nocopy_stats()['expunges'];
            if (nocopy_put($k, $v)) { $model[$k] = [$v, $e]; } else { nocopy_delete($k); unset($model[$k]); }
        } elseif ($r < 6) {
            nocopy_delete($k); unset($model[$k]);
        } elseif ($r < 8) {
            $bad += nocopy_get('shared') !== ['a' => range(1, 100)];
        } else {
            $got = nocopy_get($k, $hit);
            if ($hit ? !isset($model[$k]) || $got !== $model[$k][0] : isset($model[$k]) && nocopy_stats()['expunges'] === $model[$k][1]) $bad++;
        }
    }
    return $bad;
};
$futures = [];
for ($t = 0; $t < 8; $t++) $futures[] = \parallel\run($work, [$t]);
$bad = 0;
foreach ($futures as $f) $bad += $f->value();
unset($futures);
gc_collect_cycles();
usleep(200000); // runtimes shut their threads down
var_dump($bad, nocopy_get('shared') === ['a' => range(1, 100)], nocopy_stats()['pinned_segments'] <= $before + 1);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t023*')); ?>
--EXPECT--
int(0)
bool(true)
bool(true)
