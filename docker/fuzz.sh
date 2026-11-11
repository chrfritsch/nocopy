#!/bin/sh
# Inside the image: ASan + UBSan build, then tests/fuzz.php in copy mode and zero-copy (php -S); after each, a clear must give every segment back.
# Args: [iterations=20000] [seed=random]. NOCOPY_LEAKS=1: no ASan, the Zend allocator on (a debug PHP reports what a request leaked).
set -e
export FUZZ_VALUE NOCOPY_SO=/build/modules/nocopy.so
if [ -n "$NOCOPY_LEAKS" ]; then
  NOCOPY_CFLAGS=-DNOCOPY_DEBUG /src/docker/build.sh
else
  NOCOPY_ASAN=1 /src/docker/build.sh
  export LD_PRELOAD="$(cc -print-file-name=libasan.so)" USE_ZEND_ALLOC=0 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
fi
seed=${2:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
fail=0
for mode in copy zero; do
  rm -f /dev/shm/nocopy-fuzz.*
  N="-n -d extension=$NOCOPY_SO -d nocopy.shm_name=/nocopy-fuzz -d nocopy.shm_size=4M -d memory_limit=1G"
  php $N /src/tests/fuzz.php "${1:-20000}" "$seed" $mode > /tmp/fuzz.out 2>&1 || true
  php $N -r 'require "/src/tests/empty.inc"; $l = arena_leftovers(); echo $l ? "left after clear: " . implode(", ", $l) . "\n" : "";' >> /tmp/fuzz.out 2>&1 || true
  cat /tmp/fuzz.out
  grep -q " iterations .*, ok$" /tmp/fuzz.out && ! grep -qE "Sanitizer|runtime error|php -S died|memory leaks detected|left after clear" /tmp/fuzz.out || { echo "== fuzz $mode FAILED: ./dev ${NOCOPY_LEAKS:+leaks }fuzz ${1:-20000} $seed"; fail=1; }
done
rm -f /dev/shm/nocopy-fuzz.*
exit $fail
