#!/bin/sh
# Inside the image: loop the concurrency tests on this release build; stop at the first failure. Usage: stress.sh [loops=30]
/src/docker/test.sh >/dev/null 2>&1 || true
cd /build && export NOCOPY_SO=/build/modules/nocopy.so
for n in $(seq "${1:-30}"); do
  timeout 300 env TEST_PHP_EXECUTABLE=$(command -v php) NO_INTERACTION=1 php /usr/local/lib/php/build/run-tests.php \
    -n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-runner -q \
    tests/011_stress.phpt tests/015_parallel_writers.phpt tests/018_shift_under_readers.phpt tests/021_dead_lock_holder.phpt tests/022_merge_keeps_read.phpt >/dev/null 2>&1
  rc=$?
  for f in tests/*.diff; do [ -f "$f" ] && { echo "loop $n: $f"; cat "$f"; exit 1; }; done
  [ $rc -eq 124 ] && { echo "loop $n: timed out (hang)"; exit 1; }
done
echo "stress: $n loops clean"
