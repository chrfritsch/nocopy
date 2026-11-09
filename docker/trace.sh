#!/bin/sh
# Twitter cache traces replayed read-through at equal bytes: nocopy (bench/trace.php, trace clock via libfaketime) and libCacheSim's policies. In nocopy-trace.
# Traces (Yang et al., OSDI'20): first 20M requests of cluster4 (TTLs 60 s-1 d, mixed) and cluster42 (300/600 s), ~1 GB each:
#   mkdir -p .cache/traces; for c in 4 42; do curl -sL https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/open_source/cluster$c.sort.zst | zstd -dc | head -n 20000000 > .cache/traces/c$c.csv; done
# usage: trace.sh [raw.csv...]   env SIZES (MB, default "16 64 256"), ALGOS (cachesim's, default lru,sieve,s3fifo,lecar,arc), N (requests, default all)
/src/docker/build.sh >/dev/null || exit 1
[ $# -gt 0 ] || set -- /src/.cache/traces/*.csv
SIZES=${SIZES:-16 64 256}; ALGOS=${ALGOS:-lru,sieve,s3fifo,lecar,arc}
FT=$(find /usr/lib -name libfaketime.so.1 | head -1)
i=0
for raw in "$@"; do
  rt=/tmp/$(basename "$raw" .csv).rt
  [ -n "$N" ] && head -n "$N" "$raw" > /tmp/raw.csv && raw=/tmp/raw.csv
  # pass 1: each key's first value size and TTL; pass 2: one read-through request per get/set, size and TTL as of then
  awk -F, -v OFS=, 'NR == FNR { if ($4 > 0 && !($2 in v)) v[$2] = $4; if ($7 > 0 && !($2 in t)) t[$2] = $7; next }
    $6 == "delete" { next } { if ($4 > 0) v[$2] = $4; if ($7 > 0) t[$2] = $7 }
    $2 in v { print $1, $2, v[$2], ($2 in t ? t[$2] : 0), $3 + v[$2] }' "$raw" "$raw" > "$rt"
  echo "== $(basename "$rt"): $(wc -l < "$rt") requests"
  for mb in $SIZES; do
    i=$((i + 1))
    LD_PRELOAD=$FT FAKETIME_NO_CACHE=1 FAKETIME_DONT_FAKE_MONOTONIC=1 FAKETIME="2020-03-01 00:00:00" \
      php -n -d extension=/build/modules/nocopy.so -d memory_limit=-1 -d nocopy.shm_name=/tr$i -d nocopy.shm_size=${mb}M /src/bench/trace.php "$rt"
    rm -f /dev/shm/tr$i.*
  done
  cachesim "$rt" csv "$ALGOS" "$(for mb in $SIZES; do printf %s, $((mb * 1048576)); done | sed 's/,$//')" -t "time-col=1,obj-id-col=2,ttl-col=4,obj-size-col=5,delimiter=,,has-header=false" \
    --num-thread 4 -o /tmp/cachesim.out 2>/dev/null | sed 's/^[^ ]* //'
done
