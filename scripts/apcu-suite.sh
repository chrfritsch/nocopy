#!/bin/zsh
# Runs APCu's own .phpt suite against nocopy.
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
sed -i '' "s|\$ext = (substr(PHP_OS, 0, 3) == 'WIN') ? 'php_apcu.dll' : 'apcu.so';|\$ext = '$NOCOPY_SO';|" $W/tests/server_test.inc
cd $W && NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$PHP $PHP $RT -q \
  -d extension=$NOCOPY_SO -d nocopy.apcu_compat=1 -d apc.enable_cli=1 ${=EXTRA} tests 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^FAIL|Tests (passed|failed|skipped)"
# expected failures: apc54_014 (preload_path), apcu_sma_info (no SMA), ghbug335 (slam defense) -- all out of scope
