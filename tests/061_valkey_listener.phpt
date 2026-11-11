--TEST--
nocopy.valkey with nocopy.shm_name: one listener for every process on the arena; restarted when it dies, gone with the arena
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; if (!is_dir('/proc/self')) die('skip needs /proc'); ?>
--INI--
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t061:
nocopy.shm_name=/nocopy-t061
--CONFLICTS--
valkey
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
require __DIR__ . '/valkey.inc';
function run(string $code): string
{
    return trim((string) shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg(getenv('NOCOPY_SO'))
        . ' -d nocopy.valkey=127.0.0.1:6379 -d nocopy.valkey_prefix=t061: -d nocopy.shm_name=/nocopy-t061 -r ' . escapeshellarg($code) . ' 2>&1'));
}
function alive(int $pid): bool
{
    $st = @file_get_contents("/proc/$pid/stat");
    return $st !== false && !preg_match('/^\d+ \(.*\) Z/', $st); // a zombie (no init reaping in the container) has exited
}
apcu_clear_cache();
nocopy_put('k', 'v');
$pid = nocopy_stats()['valkey_listener'];
$seen = [];
for ($i = 0; $i < 3; $i++) {
    $seen[] = (int) run('nocopy_get("k"); echo nocopy_stats()["valkey_listener"];');
}
var_dump($pid > 0, array_unique($seen) === [$pid], substr_count(vk('CLIENT', 'LIST'), 'name=nocopy-listener'));
echo trim(file_get_contents("/proc/$pid/comm")), ' ', count(scandir("/proc/$pid/fd")) - 2, "\n"; // stdio on /dev/null, the Valkey socket, the liveness lock

// a change elsewhere reaches the arena without a call here
vk('SET', 't061:k', '5');
settle();
var_dump(nocopy_get('k'));

// killed, and a change it never sees: the next process past the stale heartbeat starts another, which clears the local copies
posix_kill($pid, 9);
vk('SET', 't061:k', '6');
usleep(1200000);
echo run('var_dump(nocopy_get("k")); echo nocopy_stats()["valkey_listener"];'), "\n";
$pid2 = nocopy_stats()['valkey_listener'];
var_dump($pid2 > 0 && $pid2 !== $pid);

// the arena removed: its listener exits
unlink('/dev/shm/' . substr(nocopy_stats()['shm_name'], 1));
usleep(300000);
var_dump(alive($pid2));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t061*')); ?>
--EXPECTF--
bool(true)
bool(true)
int(1)
nocopy-valkey 5
int(5)
int(6)
%d
bool(true)
bool(false)
