#!/bin/sh
# Inside the image: bench/tail.php per scenario for APCu, nocopy copy mode and nocopy zero-copy (64M each).
#   docker run --rm --shm-size=1g -v "$PWD":/src:ro nocopy-linux:8.5 /src/docker/tail.sh [workers=8] [secs=10] [scenarios]
. /src/docker/lib.sh
/src/docker/build.sh >/dev/null || exit 1
APCU=$(apcu_so)
W=${1:-8}; S=${2:-10}; shift 2 2>/dev/null
C="-n -d extension=pcntl -d apc.enable_cli=1 -d apc.shm_size=64M -d memory_limit=1G"
N=$(nocopy_flags tail zero_copy_seconds=0 shm_size=64M)
for s in ${*:-fit evict contend}; do
  echo "== APCu $s"; php $C -d extension=$APCU /src/bench/tail.php $s $W $S
  echo "== nocopy copy $s"; shm_rm tail; php $C $N /src/bench/tail.php $s $W $S
  echo "== nocopy zero $s"; shm_rm tail; TAIL_ZERO=1 NOCOPY_SO=$SO php $C $N /src/bench/tail.php $s $W $S
done
shm_rm tail
