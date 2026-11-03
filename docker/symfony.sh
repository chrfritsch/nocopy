#!/bin/sh
# Inside the image: build stash, run the Symfony bundle's smoke test with the APCu layer, without it, and next to real APCu.
#   docker run --rm -v "$PWD":/src:ro -v <dir with vendor/ of symfony/framework-bundle>:/sf:ro stash-linux:8.5 /src/docker/symfony.sh
set -e
/src/docker/test.sh >/dev/null 2>&1 || { /src/docker/test.sh; exit 1; }
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
run() { echo "== $*"; php -n "$@" -d extension=/build/modules/stash.so -d zend.assertions=1 /src/symfony/tests/smoke.php /sf; }
run -d apc.enable_cli=1
run -d stash.apcu_compat=0
run -d extension="$APCU"
