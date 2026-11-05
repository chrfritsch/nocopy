#!/bin/zsh
# Runs APCu's own .phpt suite against nocopy with the apcu polyfill (apcu/apcu.php, prepended to every test).
# Usage: PHP=/path/to/php NOCOPY_SO=/path/to/nocopy.so [EXTRA="-d apc.serializer=igbinary ..."] scripts/apcu-suite.sh [workdir]
# Usage note: RUN_TESTS defaults to the run-tests.php that PHP installs under <prefix>/lib/php/build
: ${PHP:?} ${NOCOPY_SO:?}
RT=${RUN_TESTS:-${PHP:h:h}/lib/php/build/run-tests.php}
[ -f $RT ] || { print -u2 "run-tests.php not found at $RT (set RUN_TESTS)"; exit 1; }
W=${1:-${TMPDIR:-/tmp}/apcu-suite}; mkdir -p $W
[ -d $W/apcu ] || git clone --depth 1 https://github.com/krakjoe/apcu.git $W/apcu
rm -rf $W/tests && cp -R $W/apcu/tests $W/tests
# our module is named "nocopy": relax the skip check, and let subprocess tests load it
print -r -- '<?php if (!function_exists("apcu_store")) die("skip apcu_* not available"); ?>' > $W/tests/skipif.inc
sed -i '' "s|\$ext = (substr(PHP_OS, 0, 3) == 'WIN') ? 'php_apcu.dll' : 'apcu.so';|\$ext = '$NOCOPY_SO -d nocopy.shm_name=/nocopy-apcu-srv -d auto_prepend_file=${0:A:h:h}/apcu/apcu.php';|" $W/tests/server_test.inc
# the tests share one arena: each starts empty, as with APCu's per-process cache
print -r -- "<?php require '${0:A:h:h}/apcu/apcu.php'; if (PHP_SAPI === 'cli') nocopy_clear();" > $W/prepend.php
cd $W && NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$PHP $PHP $RT -q \
  -d extension=$NOCOPY_SO -d nocopy.shm_name=/nocopy-apcu-suite -d auto_prepend_file=$W/prepend.php -d apc.enable_cli=1 ${=EXTRA} tests 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^FAIL|Tests (passed|failed|skipped)"
# 22 expected failures (2026-11-05, Linux): APCUIterator (iterator_*, apc54_018, ghbug185); apc.* sizing aliases (apc_026, not_enough_shm);
# preload_path (apc54_014); disabled-mode warnings (apc_disabled); per-segment sma_info (apcu_sma_info); messages and traces naming
# nocopy_* or the polyfill (apc_023, apc_099, apc_entry_002); outermost reference (apc_store_reference_php8)
