#!/bin/sh
# A/B benchmark of two source trees in one container, runs interleaved so machine noise hits both alike.
#   ./dev ab [ref] [reps] [groups], or docker run --rm --shm-size=1g -v <old>:/a:ro -v <new>:/b:ro -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/ab.sh [reps=3] [groups]; env ONLY: names by regex
# Prints the best run per measurement (min time, max rate: interference only ever slows a run down) for each tree and mode (copy = CLI, zero = under php -S via bench/zero.php).
. /src/docker/lib.sh
reps=${1:-3}; groups=${2:-read write parallel mixed contend ttl}
for t in a b; do
  rm -rf /build-$t && mkdir /build-$t && cp /$t/*.c /$t/*.h /$t/config.m4 /build-$t/ && { [ ! -d /$t/third_party ] || cp -R /$t/third_party /build-$t/; } && { [ ! -d /$t/arena ] || cp -R /$t/arena /build-$t/; }
  (cd /build-$t && phpize >/dev/null 2>&1 && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>&1) || { echo "build $t failed"; exit 1; }
done
R=/tmp/ab; rm -rf $R; mkdir -p $R
C="-n -d extension=pcntl -d apc.enable_cli=1 -d apc.shm_size=512M -d apc.entries_hint=200000"
for rep in $(seq "$reps"); do
  order="a b"; [ $((rep % 2)) -eq 0 ] && order="b a"
  for g in $groups; do for t in $order; do for m in 0 1; do
    shm_rm bench; f=run; [ $m = 1 ] && f=zero; SO=/build-$t/modules/nocopy.so; NOCOPY_SO=$SO php $C $(nocopy_flags bench zero_copy_seconds=0 shm_size=512M entries_hint=200000) /src/bench/$f.php $g >> $R/$t$m.jsonl 2>/dev/null
  done; done; done
done
php -n -r '
foreach (["a0", "b0", "a1", "b1"] as $c) foreach (file("/tmp/ab/$c.jsonl") as $l) { $j = json_decode($l, true); if ($j) { $d[$j["name"]][$c][] = $j["v"]; $u[$j["name"]] = $j["unit"]; } }
$best = fn($a, $hi) => $hi ? max($a) : min($a);
printf("%-46s %10s %10s %7s | %10s %10s %7s\n", "", "old copy", "new copy", "", "old zero", "new zero", "");
foreach ($d as $n => $x) {
  $hi = str_contains($u[$n], "/s"); $o = "";
  foreach (["0", "1"] as $k) { $p = $best($x["a$k"], $hi); $q = $best($x["b$k"], $hi); $r = $p && $q ? ($hi ? $q / $p - 1 : $p / $q - 1) * 100 : 0;
    $o .= sprintf("%10s %10s %+6.1f%% ", round($p, 2), round($q, 2), $r) . ($k === "0" ? "| " : ""); }
  printf("%-46s %s\n", "$n ($u[$n])", $o);
}'
