#!/bin/sh
# Inside the image: build nocopy, run the Symfony bundle's smoke test with the APCu layer, without it, and next to real APCu.
#   docker run --rm -v "$PWD":/src:ro -v <dir with vendor/ of symfony/framework-bundle>:/sf:ro nocopy-linux:8.5 /src/docker/symfony.sh
set -e
/src/docker/test.sh >/dev/null 2>&1 || { /src/docker/test.sh; exit 1; }
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
run() { echo "== $*"; rm -f /dev/shm/nocopy-sf.*; php -n "$@" -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-sf -d zend.assertions=1 /src/symfony/tests/smoke.php /sf; }
run -d nocopy.apcu_compat=1 -d apc.enable_cli=1
run
run -d extension="$APCU"
