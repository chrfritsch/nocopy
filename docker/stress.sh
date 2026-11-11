#!/bin/sh
# Inside the image: loop the concurrency tests on this release build; stop at the first failure. Usage: stress.sh [loops=30]
. /src/docker/lib.sh
/src/docker/build.sh && cp -r /src/tests /build/tests && cp -r /src/apcu /build/apcu || exit 1
cd /build && export NOCOPY_SO=$SO
for n in $(seq "${1:-30}"); do
  TIMEOUT=300 run_tests tests/011_stress.phpt tests/015_parallel_writers.phpt tests/018_shift_under_readers.phpt tests/021_dead_lock_holder.phpt tests/022_merge_keeps_read.phpt >/dev/null 2>&1
  rc=$?
  for f in tests/*.diff; do [ -f "$f" ] && { echo "loop $n: $f"; cat "$f"; exit 1; }; done
  [ $rc -eq 124 ] && { echo "loop $n: timed out (hang)"; exit 1; }
done
echo "stress: $n loops clean"
