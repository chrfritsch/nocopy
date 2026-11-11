#!/bin/sh
# Inside the image: build /src out of tree in /build, run the own tests (args: extra run-tests options or test paths). Exit 1 on any failure.
# NOCOPY_CONFIGURE: extra configure flags, e.g. --disable-nocopy-valkey
set -e
. /src/docker/lib.sh
/src/docker/build.sh
cd /build
cp -r /src/tests /build/tests && cp -r /src/apcu /build/apcu && cp -r /src/cli /build/cli
# Valkey tests (05x) need a server on 127.0.0.1:6379; they skip without one
valkey_up
export NOCOPY_SO=$SO
[ -z "$NOCOPY_ASAN" ] || asan_env
run_tests -j"$(nproc)" "${@:-/build/tests}" > /build/run.out 2>&1 || true
grep -E "^(FAIL|SKIP|BORK)|XFAIL REASON|Tests (passed|failed|skipped)" /build/run.out || true
bad=0
grep -q "^Tests passed" /build/run.out || { tail -20 /build/run.out; bad=1; }
grep -q "^BORK" /build/run.out && bad=1
for f in /build/tests/*.diff; do
  [ -f "$f" ] || continue
  grep -q "XFAIL.*/$(basename "${f%.diff}").phpt" /build/run.out && continue # an expected failure (--XFAIL--) leaves its diff too
  echo "== $f"; head -20 "$f"; bad=1
done
exit $bad
