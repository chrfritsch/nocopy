#!/bin/sh
# Runs APCu's own .phpt suite against nocopy with the apcu polyfill (apcu/apcu.php, prepended to every test). `./dev apcu` runs it in Docker.
# Usage: [PHP=php] [NOCOPY_SO=/build/modules/nocopy.so] [EXTRA="-d apc.serializer=igbinary ..."] scripts/apcu-suite.sh [workdir]
PHP=$(command -v "${PHP:-php}"); NOCOPY_SO=${NOCOPY_SO:-/build/modules/nocopy.so}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RT=${RUN_TESTS:-$(dirname "$(dirname "$PHP")")/lib/php/build/run-tests.php}
[ -f "$RT" ] || { echo "run-tests.php not found at $RT (set RUN_TESTS)" >&2; exit 1; }
W=${1:-${TMPDIR:-/tmp}/apcu-suite}; mkdir -p "$W"
[ -d "$W/apcu" ] || git clone -q --depth 1 https://github.com/krakjoe/apcu.git "$W/apcu"
rm -rf "$W/tests" && cp -R "$W/apcu/tests" "$W/tests"
# our module is named "nocopy": relax the skip check, and let subprocess tests load it
echo '<?php if (!function_exists("apcu_store")) die("skip apcu_* not available"); ?>' > "$W/tests/skipif.inc"
sed -i.bak "s|\$ext = (substr(PHP_OS, 0, 3) == 'WIN') ? 'php_apcu.dll' : 'apcu.so';|\$ext = '$NOCOPY_SO -d nocopy.shm_name=/nocopy-apcu-srv -d auto_prepend_file=$ROOT/apcu/apcu.php';|" "$W/tests/server_test.inc"
# the tests share one arena: each starts empty, as with APCu's per-process cache
echo "<?php require '$ROOT/apcu/apcu.php'; if (PHP_SAPI === 'cli') nocopy_clear();" > "$W/prepend.php"
cd "$W" && NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$PHP $PHP "$RT" -q \
  -d extension="$NOCOPY_SO" -d nocopy.shm_name=/nocopy-apcu-suite -d auto_prepend_file="$W/prepend.php" -d apc.enable_cli=1 $EXTRA tests 2>&1 | sed 's/\x1b\[[0-9;]*m//g' \
  | awk '/^Tests (passed|failed|skipped)/; /^FAILED TEST SUMMARY/ {f=1; next} /^=+$/ {f=0} f' 
# 24 expected failures (2026-11-06, Linux): APCUIterator (iterator_*, apc54_018, ghbug185); apc.* sizing aliases (apc_026, not_enough_shm);
# apc.slam_defense (ghbug335, runs only with pcntl);
# preload_path (apc54_014); disabled-mode warnings (apc_disabled); per-segment sma_info (apcu_sma_info); messages and traces naming
# nocopy_* or the polyfill (apc_023, apc_099, apc_entry_002); outermost reference (apc_store_reference_php8);
# debug_zval_dump of shared references: arrays come back packed like the stored ones, APCu unserializes them into hashes (apc_006_php81)
