#!/bin/sh
# Inside the image: bench/stampede.php for APCu (naive, apcu_entry) and nocopy (naive, apcu_entry, nocopy_remember).
#   docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/stampede.sh [workers=8] [secs=10] [ttl=2] [compute_ms=50]
. /src/docker/lib.sh
/src/docker/build.sh >/dev/null
APCU=$(apcu_so)
C="-n -d extension=pcntl -d apc.enable_cli=1 -d apc.shm_size=64M"
for m in naive entry; do echo "== APCu $m"; php $C -d extension=$APCU /src/bench/stampede.php $m "$@"; done
for m in naive entry remember; do echo "== nocopy $m"; shm_rm bench; php $C $(nocopy_flags bench shm_size=64M) /src/bench/stampede.php $m "$@"; done
