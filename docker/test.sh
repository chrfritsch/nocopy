#!/bin/sh
# Inside the image: build /src out of tree in /build, run the own tests (args: extra run-tests options or test paths). Exit 1 on any failure.
set -e
rm -rf /build && mkdir /build && cd /src
cp *.c *.h config.m4 config.w32 /build/
cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>make.err || { cat make.err; exit 1; }
grep -i warning make.err || true
cp -r /src/tests /build/tests && cp -r /src/apcu /build/apcu
# Valkey tests (05x) need a server on 127.0.0.1:6379; they skip without one
command -v valkey-server >/dev/null && ! valkey-cli ping >/dev/null 2>&1 && valkey-server --daemonize yes --save '' --appendonly no >/dev/null
export NOCOPY_SO=/build/modules/nocopy.so
TEST_PHP_EXECUTABLE=$(command -v php) NO_INTERACTION=1 php /usr/local/lib/php/build/run-tests.php \
  -n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-runner -q "${@:-/build/tests}" 2>&1 | grep -E "^(FAIL|SKIP|BORK)|Tests (passed|failed|skipped)" || true
bad=0
for f in /build/tests/*.diff; do [ -f "$f" ] && { echo "== $f"; head -20 "$f"; bad=1; }; done
exit $bad
