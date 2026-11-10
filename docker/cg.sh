#!/bin/sh
# Inside nocopy-perf: cachegrind per-op cost of bench/prof.php ops (runs of n and 2n, cg_diff), top functions; data in /out.
#   OPS="put_arr put_prods" N=20000 TOP=15 ANNOTATE=1 (per source line of the top functions)
set -e
/src/docker/build.sh
C="-n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-cg -d nocopy.shm_size=128M -d nocopy.entries_hint=200000"
n=${N:-20000}
for op in ${OPS:-put_int put_str put_arr put_big put_obj put_prods put_ords put_json}; do
  for k in 1 2; do
    rm -f /dev/shm/nocopy-cg*
    valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/out/cg-$op-$k.out php $C /src/bench/prof.php $op copy $((n * k)) >/dev/null 2>&1
  done
  cg_diff /out/cg-$op-1.out /out/cg-$op-2.out > /out/cg-$op.diff
  echo "== $op (Ir for $n ops)"
  cg_annotate --show-percs=no /out/cg-$op.diff 2>/dev/null | sed -n '/File:function/,/^-- Function:/p' | grep -E '^ +[0-9,]+ +[a-z_]' | awk -v n=$n '{gsub(",","",$1); printf "%8.1f Ir/op  %s\n", $1 / n, $2}' | head -n "${TOP:-12}"
  [ -z "$ANNOTATE" ] || cg_annotate --auto=yes --show-percs=no /out/cg-$op.diff 2>/dev/null | grep -E '^ +[0-9,]+ .*(nocopy|\.c)' | sort -t, -k1 | head -n 40
done
