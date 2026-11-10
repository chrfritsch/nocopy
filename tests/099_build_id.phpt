--TEST--
nocopy: the layout id is the loaded code's build id, not a hash of the file on disk: the same code from two different files shares one arena (design review 2026-11, finding 4)
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t099
--FILE--
<?php
$dir = sys_get_temp_dir() . '/nocopy-t099-' . getmypid();
mkdir($dir);
copy(getenv('NOCOPY_SO'), "$dir/a.so");
copy(getenv('NOCOPY_SO'), "$dir/b.so");
file_put_contents("$dir/b.so", str_repeat("\0", 4096), FILE_APPEND); // another file, the same code (what a package upgrade's rewrite can look like)
$run = fn($so, $code) => trim((string) shell_exec(escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg($so)
    . ' -d nocopy.shm_name=/nocopy-t099b -r ' . escapeshellarg($code) . ' 2>&1'));
$a = $run("$dir/a.so", 'nocopy_put("k", "from a"); echo nocopy_stats()["shm_name"];');
$b = $run("$dir/b.so", 'echo nocopy_stats()["shm_name"], " ", nocopy_get("k") ?? "miss";');
echo $a === explode(' ', $b)[0] ? "same arena" : "different arenas: $a / $b", "\n", explode(' ', $b, 2)[1] ?? '', "\n";
unlink("$dir/a.so");
unlink("$dir/b.so");
rmdir($dir);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t099*')); ?>
--EXPECT--
same arena
from a
