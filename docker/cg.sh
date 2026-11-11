#!/bin/sh
# Inside nocopy-perf: cachegrind per-op cost of bench/prof.php ops (runs of n and 2n, cg_diff), top functions; data in /out.
#   OPS="put_arr put_prods" N=20000; table: Ir per call and the biggest functions (docker/cg-report.sh)
set -e
/src/docker/build.sh
C="-n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-cg -d nocopy.shm_size=128M -d nocopy.entries_hint=200000"
n=${N:-20000}
O="${OPS:-get_int get_hit get_ttl get_str get_arr get_miss get_multi get_obj get_prods remember remember_miss put_int put_ttl put_str put_arr put_obj put_prods put_multi put_del add_fail add_del inc inc_ttl dec cas_ok cas_fail delete_miss delete_multi_miss putdel_multi keys stats shared_with}"
for op in $O; do
  for k in 1 2; do
    rm -f /dev/shm/nocopy-cg*
    valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/out/cg-$op-$k.out php $C /src/bench/prof.php $op copy $((n * k)) >/dev/null 2>&1
  done
  cg_diff /out/cg-$op-1.out /out/cg-$op-2.out > /out/cg-$op.diff
done
OPS="$O" /src/docker/cg-report.sh
