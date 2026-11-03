#!/bin/sh
# Inside the image: build /src out of tree, run the own tests. Prints the .so path last.
set -e
rm -rf /build && mkdir /build && cd /src
cp stash.c php_stash.h stash_arginfo.h config.m4 config.w32 /build/
cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>make.err || { cat make.err; exit 1; }
grep -i warning make.err || true
cp -r /src/tests /build/tests
TEST_PHP_EXECUTABLE=$(command -v php) NO_INTERACTION=1 php /usr/local/lib/php/build/run-tests.php \
  -n -d extension=/build/modules/stash.so -q /build/tests "$@" 2>&1 | grep -E "^(FAIL|SKIP|BORK)|Tests (passed|failed|skipped)" || true
for f in /build/tests/*.diff; do [ -f "$f" ] && { echo "== $f"; head -20 "$f"; }; done
echo "so: /build/modules/stash.so"
