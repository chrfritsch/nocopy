#!/bin/zsh
# Runs every benchmark group against APCu and stash; writes bench/results/<config>.jsonl
# Usage: PHP=/path/to/release/php STASH_SO=... APCU_SO=... bench/all.sh
: ${PHP:?set PHP to a release (non-debug) php binary} ${STASH_SO:?} ${APCU_SO:?}
HERE=${0:A:h}
mkdir -p $HERE/results
COMMON="-d apc.enable_cli=1 -d apc.shm_size=512M -d apc.entries_hint=200000"
run_cfg() { name=$1; shift; : > $HERE/results/$name.jsonl
  for g in read write parallel mixed contend ttl; do $PHP "$@" ${=COMMON} $HERE/run.php $g >> $HERE/results/$name.jsonl 2>>$HERE/results/$name.err; done; }
run_cfg apcu_php     -d extension=$APCU_SO -d apc.serializer=php
run_cfg apcu_default -d extension=$APCU_SO -d apc.serializer=default
run_cfg stash_zero   -d extension=$STASH_SO -d stash.request_scope=1
run_cfg stash_copy   -d extension=$STASH_SO -d stash.request_scope=0
$PHP $HERE/table.php
