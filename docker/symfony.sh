#!/bin/sh
# Inside the image: build nocopy, run the Symfony bundle's smoke test alone and next to real APCu.
#   docker run --rm -v "$PWD":/src:ro -v <dir with vendor/ of symfony/framework-bundle>:/sf:ro nocopy-linux:8.5 /src/docker/symfony.sh
set -e
. /src/docker/lib.sh
/src/docker/build.sh || exit 1
resp_up
APCU=$(apcu_so)
run() { echo "== $*"; shm_rm sf; php -n "$@" $(nocopy_flags sf) -d zend.assertions=1 /src/symfony/tests/smoke.php /sf; }
run
run -d extension="$APCU"
echo "== no nocopy.shm_name"; php -n -d extension=$SO -d zend.assertions=1 /src/symfony/tests/smoke.php /sf
run -d nocopy.resp=127.0.0.1:6379 -d nocopy.resp_prefix=sf:
# lifetime 0 must not mean forever in RESP: versioned-out keys would never leave
keys=$(valkey-cli --scan --pattern 'sf:*'); [ -n "$keys" ] || { echo "nothing reached RESP"; exit 1; }
for k in $keys; do [ "$(valkey-cli ttl "$k")" -gt 0 ] || { echo "no TTL on $k"; exit 1; }; done
