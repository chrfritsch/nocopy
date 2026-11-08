#!/bin/sh
# Inside the image: ASan + UBSan build, then tests/fuzz.php in copy mode and zero-copy (php -S). Args: [iterations=20000] [seed=random].
set -e
NOCOPY_ASAN=1 /src/docker/build.sh
export FUZZ_VALUE; export NOCOPY_SO=/build/modules/nocopy.so LD_PRELOAD="$(cc -print-file-name=libasan.so)" USE_ZEND_ALLOC=0 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
seed=${2:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
fail=0
for mode in copy zero; do
  rm -f /dev/shm/nocopy-fuzz.*
  php -n -d extension=$NOCOPY_SO -d nocopy.shm_name=/nocopy-fuzz -d nocopy.shm_size=4M -d memory_limit=1G /src/tests/fuzz.php "${1:-20000}" "$seed" $mode > /tmp/fuzz.out 2>&1 || true
  cat /tmp/fuzz.out
  grep -q " iterations .*, ok$" /tmp/fuzz.out && ! grep -qE "Sanitizer|runtime error|php -S died" /tmp/fuzz.out || { echo "== fuzz $mode FAILED: ./dev fuzz ${1:-20000} $seed"; fail=1; }
done
rm -f /dev/shm/nocopy-fuzz.*
exit $fail
