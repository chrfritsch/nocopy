#!/bin/sh
# Inside the image: build, then bench/run.php groups (args, default all; env ONLY: names by regex) x3 for APCu and nocopy; prints medians (APCu default serializer, nocopy copy, nocopy zero-copy).
#   docker run --rm --shm-size=1g -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/bench.sh
/src/docker/build.sh || exit 1
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
R=/tmp/res; mkdir -p $R
C="-n -d extension=pcntl -d apc.enable_cli=1 -d apc.shm_size=512M -d apc.entries_hint=200000"
for rep in 1 2 3; do
  for g in ${*:-read write parallel mixed contend ttl}; do
    php $C -d extension=$APCU /src/bench/run.php $g >> $R/apcu.jsonl 2>/dev/null
    rm -f /dev/shm/nocopy-bench.*; php $C -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-bench -d nocopy.zero_copy_seconds=0 -d nocopy.shm_size=512M -d nocopy.entries_hint=200000 /src/bench/run.php $g >> $R/copy.jsonl 2>/dev/null
    rm -f /dev/shm/nocopy-bench.*; NOCOPY_SO=/build/modules/nocopy.so php $C -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-bench -d nocopy.zero_copy_seconds=0 -d nocopy.shm_size=512M -d nocopy.entries_hint=200000 /src/bench/zero.php $g >> $R/zero.jsonl 2>/dev/null
  done
done
php -n -r '
foreach (["apcu", "copy", "zero"] as $c) foreach (file("/tmp/res/$c.jsonl") as $l) { $j = json_decode($l, true); if ($j) { $d[$j["name"]][$c][] = $j["v"]; $u[$j["name"]] = $j["unit"]; } }
$m = function ($a) { if (!$a) return "-"; sort($a); return round($a[intdiv(count($a), 2)], 2); };
printf("%-46s %12s %12s %12s\n", "", "apcu", "nocopy copy", "nocopy zero");
foreach ($d as $n => $x) printf("%-46s %12s %12s %12s\n", "$n ($u[$n])", $m($x["apcu"] ?? []), $m($x["copy"] ?? []), $m($x["zero"] ?? []));'
