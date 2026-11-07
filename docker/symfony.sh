#!/bin/sh
# Inside the image: build nocopy, run the Symfony bundle's smoke test alone and next to real APCu.
#   docker run --rm -v "$PWD":/src:ro -v <dir with vendor/ of symfony/framework-bundle>:/sf:ro nocopy-linux:8.5 /src/docker/symfony.sh
set -e
/src/docker/build.sh || exit 1
valkey-cli ping >/dev/null 2>&1 || valkey-server --daemonize yes --save '' --appendonly no >/dev/null
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
run() { echo "== $*"; rm -f /dev/shm/nocopy-sf.*; php -n "$@" -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-sf -d zend.assertions=1 /src/symfony/tests/smoke.php /sf; }
run
run -d extension="$APCU"
echo "== no nocopy.shm_name"; php -n -d extension=/build/modules/nocopy.so -d zend.assertions=1 /src/symfony/tests/smoke.php /sf
run -d nocopy.valkey=127.0.0.1:6379 -d nocopy.valkey_prefix=sf:
# lifetime 0 must not mean forever in Valkey: versioned-out keys would never leave
keys=$(valkey-cli --scan --pattern 'sf:*'); [ -n "$keys" ] || { echo "nothing reached Valkey"; exit 1; }
for k in $keys; do [ "$(valkey-cli ttl "$k")" -gt 0 ] || { echo "no TTL on $k"; exit 1; }; done
