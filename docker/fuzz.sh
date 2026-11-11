#!/bin/sh
# Inside the image: ASan + UBSan build, then tests/fuzz.php in copy mode and zero-copy (php -S); after each, a clear must give every segment back.
# Args: [iterations=20000] [seed=random]. NOCOPY_LEAKS=1: no ASan, the Zend allocator on (a debug PHP reports what a request leaked).
set -e
. /src/docker/lib.sh
export FUZZ_VALUE NOCOPY_SO=$SO
if [ -n "$NOCOPY_LEAKS" ]; then
  NOCOPY_CFLAGS=-DNOCOPY_DEBUG /src/docker/build.sh
else
  NOCOPY_ASAN=1 /src/docker/build.sh
  asan_env
fi
seed=${2:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
fail=0
for mode in copy zero; do
  shm_rm fuzz
  N="-n $(nocopy_flags fuzz shm_size=4M) -d memory_limit=1G"
  php $N /src/tests/fuzz.php "${1:-20000}" "$seed" $mode > /tmp/fuzz.out 2>&1 || true
  php $N -r 'require "/src/tests/empty.inc"; $l = arena_leftovers(); echo $l ? "left after clear: " . implode(", ", $l) . "\n" : "";' >> /tmp/fuzz.out 2>&1 || true
  cat /tmp/fuzz.out
  grep -q " iterations .*, ok$" /tmp/fuzz.out && ! grep -qE "Sanitizer|runtime error|php -S died|memory leaks detected|left after clear" /tmp/fuzz.out || { echo "== fuzz $mode FAILED: ./dev ${NOCOPY_LEAKS:+leaks }fuzz ${1:-20000} $seed"; fail=1; }
done
shm_rm fuzz
exit $fail
