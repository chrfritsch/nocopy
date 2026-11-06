#!/bin/sh
# Inside nocopy-perf (--privileged): build, perf record bench/prof.php per op and mode, print the top symbols; data in /out.
#   OPS="get_int put_int" MODES="copy zero" ANNOTATE="nocopy_fetch ..." (share of the function's samples per source line) TOP=8 LINES=15
set -e
SO=/build/modules/nocopy.so
# Samples of function $1 in $2 by source line (perf annotate -l crashes on this binary): "share% file:line".
lines() {
  base=$(nm $SO | awk -v s="$1" '$3 == s {print $1; exit}'); [ -n "$base" ] || return 0
  [ -s "$2.script" ] || perf script --no-inline -i "$2" -F ip,sym,symoff,dso > "$2.script" 2>/dev/null || true
  sed -n "s/.* $1+0x\([0-9a-f]*\) .*/\1/p" "$2.script" | sort | uniq -c | while read -r n off; do
    printf '%s %s\n' "$n" "$(addr2line -e $SO "$(printf '%x' $((0x$base + 0x$off)))" | sed 's|.*/||; s/ (discriminator.*//')"
  done | awk '{c[$2] += $1; t += $1} END {for (l in c) printf "%6.1f%% %s\n", 100 * c[l] / t, l}' | sort -rn | head -n "${LINES:-15}"
}
/src/docker/build.sh
echo -1 > /proc/sys/kernel/perf_event_paranoid; echo 0 > /proc/sys/kernel/kptr_restrict
C="-n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-prof -d nocopy.shm_size=128M -d nocopy.entries_hint=200000"
for op in ${OPS:-get_int get_ttl get_str get_arr get_big get_miss get_100k get_multi remember put_int put_ttl put_str put_arr put_big put_obj put_del put_100k inc add_fail}; do
  for m in ${MODES:-copy zero}; do
    rm -f /dev/shm/nocopy-prof* /out/$op-$m.data.script
    f=/out/$op-$m
    NOCOPY_SO=/build/modules/nocopy.so perf record -q -F 4999 -o $f.data -- php $C /src/bench/prof.php $op $m 2>/dev/null | tail -1
    perf report -i $f.data --no-children --sort dso,sym --stdio 2>/dev/null | grep -E '^ +[0-9]' | grep -v ' 0x0000' > $f.txt || true
    head -n "${TOP:-8}" $f.txt | sed 's/^/    /'
    for s in $ANNOTATE; do echo "    -- $s by line"; lines "$s" $f.data | tee $f.$s.lines | sed 's/^/    /'; done
  done
done
