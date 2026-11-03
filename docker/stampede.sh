#!/bin/sh
# Inside the image: bench/stampede.php for APCu (naive, apcu_entry) and stash (naive, apcu_entry, stash_remember).
#   docker run --rm -v "$PWD":/src:ro stash-linux:8.5 /src/docker/stampede.sh [workers=8] [secs=10] [ttl=2] [compute_ms=50]
/src/docker/test.sh /src/tests/001_roundtrip.phpt >/dev/null 2>&1 # builds /build/modules/stash.so
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
C="-n -d extension=pcntl -d apc.enable_cli=1 -d apc.shm_size=64M"
for m in naive entry; do echo "== APCu $m"; php $C -d extension=$APCU /src/bench/stampede.php $m "$@"; done
for m in naive entry remember; do echo "== stash $m"; php $C -d extension=/build/modules/stash.so /src/bench/stampede.php $m "$@"; done
