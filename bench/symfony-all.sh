#!/bin/zsh
# Usage: PHP=... STASH_SO=... APCU_SO=... SF_AUTOLOAD=.../vendor/autoload.php bench/symfony-all.sh
: ${PHP:?} ${STASH_SO:?} ${APCU_SO:?} ${SF_AUTOLOAD:?}
HERE=${0:A:h}; export SF_AUTOLOAD
mkdir -p $HERE/results-sf
COMMON="-d apc.enable_cli=1 -d apc.shm_size=512M -d apc.entries_hint=200000 -d memory_limit=-1"
run_cfg() { name=$1; shift; : > $HERE/results-sf/$name.jsonl
  for g in read write parallel; do $PHP "$@" ${=COMMON} $HERE/symfony.php $g >> $HERE/results-sf/$name.jsonl 2>>$HERE/results-sf/$name.err; done; }
run_cfg apcu_php     -d extension=$APCU_SO -d apc.serializer=php
run_cfg apcu_default -d extension=$APCU_SO -d apc.serializer=default
run_cfg stash_zero   -d extension=$STASH_SO -d stash.request_scope=1
run_cfg stash_copy   -d extension=$STASH_SO -d stash.request_scope=0
$PHP $HERE/table.php results-sf
