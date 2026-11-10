#!/bin/sh
# Inside the image: build /src out of tree in /build, run the own tests (args: extra run-tests options or test paths). Exit 1 on any failure.
# NOCOPY_CONFIGURE: extra configure flags, e.g. --disable-nocopy-valkey
set -e
/src/docker/build.sh
cd /build
cp -r /src/tests /build/tests && cp -r /src/apcu /build/apcu && cp -r /src/cli /build/cli
# Valkey tests (05x) need a server on 127.0.0.1:6379; they skip without one
command -v valkey-server >/dev/null && ! valkey-cli ping >/dev/null 2>&1 && valkey-server --daemonize yes --save '' --appendonly no >/dev/null
export NOCOPY_SO=/build/modules/nocopy.so
# php itself is not instrumented: preload the runtime, plain malloc so emalloc overflows are seen, no leak report (php frees lazily)
[ -n "$NOCOPY_ASAN" ] && export LD_PRELOAD="$(cc -print-file-name=libasan.so)" USE_ZEND_ALLOC=0 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
TEST_PHP_EXECUTABLE=$(command -v php) NO_INTERACTION=1 php "$(php-config --prefix)/lib/php/build/run-tests.php" \
  -n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-runner -q -j"$(nproc)" "${@:-/build/tests}" > /build/run.out 2>&1 || true
grep -E "^(FAIL|SKIP|BORK)|Tests (passed|failed|skipped)" /build/run.out || true
bad=0
grep -q "^Tests passed" /build/run.out || { tail -20 /build/run.out; bad=1; }
grep -q "^BORK" /build/run.out && bad=1
for f in /build/tests/*.diff; do [ -f "$f" ] && { echo "== $f"; head -20 "$f"; bad=1; }; done
exit $bad
