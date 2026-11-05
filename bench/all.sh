#!/bin/zsh
# Runs every benchmark group against APCu and nocopy; writes bench/results/<config>.jsonl
# Usage: PHP=/path/to/release/php NOCOPY_SO=... APCU_SO=... bench/all.sh
: ${PHP:?set PHP to a release (non-debug) php binary} ${NOCOPY_SO:?} ${APCU_SO:?}
HERE=${0:A:h}
mkdir -p $HERE/results
COMMON="-d apc.enable_cli=1 -d apc.shm_size=512M -d apc.entries_hint=200000"
run_cfg() { name=$1; shift; : > $HERE/results/$name.jsonl
  for g in read write parallel mixed contend ttl; do $PHP "$@" ${=COMMON} $HERE/run.php $g >> $HERE/results/$name.jsonl 2>>$HERE/results/$name.err; done; }
run_cfg apcu_php     -d extension=$APCU_SO -d apc.serializer=php
run_cfg apcu_default -d extension=$APCU_SO -d apc.serializer=default
run_cfg nocopy_zero   -d extension=$NOCOPY_SO -d nocopy.apcu_compat=1 -d nocopy.request_scope=1
run_cfg nocopy_copy   -d extension=$NOCOPY_SO -d nocopy.apcu_compat=1 -d nocopy.request_scope=0
$PHP $HERE/table.php
