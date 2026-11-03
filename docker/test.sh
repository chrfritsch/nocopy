#!/bin/sh
# Inside the image: build /src out of tree in /build, run the own tests (args: extra run-tests options or test paths). Exit 1 on any failure.
set -e
rm -rf /build && mkdir /build && cd /src
cp stash.c stash_apcu.c stash_arena.c stash_arena.h stash_php.h php_stash.h stash_arginfo.h config.m4 config.w32 /build/
cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>make.err || { cat make.err; exit 1; }
grep -i warning make.err || true
cp -r /src/tests /build/tests
TEST_PHP_EXECUTABLE=$(command -v php) NO_INTERACTION=1 php /usr/local/lib/php/build/run-tests.php \
  -n -d extension=/build/modules/stash.so -q "${@:-/build/tests}" 2>&1 | grep -E "^(FAIL|SKIP|BORK)|Tests (passed|failed|skipped)" || true
bad=0
for f in /build/tests/*.diff; do [ -f "$f" ] && { echo "== $f"; head -20 "$f"; bad=1; }; done
exit $bad
