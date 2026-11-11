<?php
// Other PHP processes with this build of nocopy (NOCOPY_SO, or $so), for the tests: php -n, nocopy loaded, $ini (name => value), then $args.
function php_argv(array $ini = [], array $args = [], ?string $so = null): array
{
    $cmd = [PHP_BINARY, '-n', '-d', 'extension=' . ($so ?? getenv('NOCOPY_SO'))];
    foreach ($ini as $k => $v) {
        array_push($cmd, '-d', "$k=$v");
    }
    return [...$cmd, ...$args];
}

// The same as a shell command line.
function php_cmd(array $ini = [], array $args = [], ?string $so = null): string
{
    return implode(' ', array_map('escapeshellarg', php_argv($ini, $args, $so)));
}

// Runs $code (php -r) in one; returns its output, stderr included. $before: what the shell puts in front (setpriv, environment).
function php_run(string $code, array $ini = [], string $before = '', ?string $so = null): string
{
    return (string) shell_exec($before . php_cmd($ini, ['-r', $code], $so) . ' 2>&1');
}
