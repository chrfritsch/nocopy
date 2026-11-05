<?php
// php [-d nocopy.*] zero.php <group>: run.php <group> as one php -S request (short-lived SAPI: zero-copy). Needs NOCOPY_SO.
require __DIR__ . '/../tests/serve.inc';
echo serve('<?php $argv = ["run.php", ' . var_export($argv[1], true) . ']; require ' . var_export(__DIR__ . '/run.php', true) . ';', ['extension' => 'pcntl']);
