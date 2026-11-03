--TEST--
stash (ZTS): threads share the arena; each claims its own worker slot and gives it back when it ends
--EXTENSIONS--
stash
parallel
--INI--
apc.shm_size=64M
stash.request_scope=1
--FILE--
<?php
stash_put('shared', ['a' => range(1, 100)]);
$before = stash_stats()['pinned_segments'];
$work = function (int $t) {
    mt_srand($t);
    $model = []; $bad = 0;
    for ($i = 0; $i < 20000; $i++) {
        $k = "t{$t}_" . mt_rand(0, 200);
        $r = mt_rand(0, 9);
        if ($r < 4) {
            $v = array_fill(0, [1, 10, 100, 300][mt_rand(0, 3)], "v$t.$i");
            $e = stash_stats()['expunges'];
            if (stash_put($k, $v)) { $model[$k] = [$v, $e]; } else { stash_delete($k); unset($model[$k]); }
        } elseif ($r < 6) {
            stash_delete($k); unset($model[$k]);
        } elseif ($r < 8) {
            $bad += stash_get('shared') !== ['a' => range(1, 100)];
        } else {
            $got = stash_get($k, $hit);
            if ($hit ? !isset($model[$k]) || $got !== $model[$k][0] : isset($model[$k]) && stash_stats()['expunges'] === $model[$k][1]) $bad++;
        }
        if ($i % 50 == 0) stash_checkpoint(); // end of a "request": zero-copy pins go
    }
    stash_checkpoint(); // parallel keeps its threads (and their requests) alive after a task
    return $bad;
};
$futures = [];
for ($t = 0; $t < 8; $t++) $futures[] = \parallel\run($work, [$t]);
$bad = 0;
foreach ($futures as $f) $bad += $f->value();
unset($futures);
gc_collect_cycles();
usleep(200000); // runtimes shut their threads down
var_dump($bad, stash_get('shared') === ['a' => range(1, 100)], stash_stats()['pinned_segments'] <= $before + 1);
?>
--EXPECT--
int(0)
bool(true)
bool(true)
