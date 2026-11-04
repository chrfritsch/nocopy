#!/bin/sh
# Inside nocopy-drupal: build nocopy, run the Drupal module's PHPUnit tests in copy and zero-copy mode. /drupal caches the Drupal checkout.
#   docker run --rm -v "$PWD":/src:ro -v nocopy-drupal-test:/drupal nocopy-drupal /src/docker/drupal-test.sh
# EXTRA: more INI lines, e.g. EXTRA="nocopy.shm_size=1M"
/src/docker/test.sh >/dev/null 2>&1 || { echo "nocopy build/tests failed"; /src/docker/test.sh; exit 1; }
export COMPOSER_ALLOW_SUPERUSER=1 COMPOSER_NO_INTERACTION=1
if [ ! -d /drupal/web ]; then
  composer create-project -q drupal/recommended-project:^11 /drupal/tmp && mv /drupal/tmp/* /drupal/tmp/.[!.]* /drupal/ 2>/dev/null; rmdir /drupal/tmp
  (cd /drupal && composer require -q -W --dev drupal/core-dev) || exit 1
fi
rm -rf /drupal/web/modules/contrib/nocopy && mkdir -p /drupal/web/modules/contrib && cp -r /src/drupal /drupal/web/modules/contrib/nocopy
cd /drupal
rc=0
# kernel tests run in child processes, which do not inherit -d flags: settings go to conf.d
for mode in "" "nocopy.request_scope=1"; do
  echo "== mode: ${mode:-copy} $EXTRA"
  printf '%s\n' "extension=/build/modules/nocopy.so" "extension=igbinary" "nocopy.serializer=igbinary" "memory_limit=1G" $mode $EXTRA > /usr/local/etc/php/conf.d/zz-nocopy.ini
  rm -f /tmp/t.sqlite*
  SIMPLETEST_DB=sqlite://localhost//tmp/t.sqlite SIMPLETEST_BASE_URL=http://localhost \
    php vendor/bin/phpunit -c web/core --fail-on-skipped --display-skipped web/modules/contrib/nocopy/tests || rc=1
done
exit $rc
