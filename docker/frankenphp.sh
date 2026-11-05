#!/bin/sh
# FrankenPHP (ZTS, threads): classic requests, worker mode under "auto" and with nocopy.request_scope=1 (both copy mode).
#   docker run --rm -v "$PWD":/src:ro dunglas/frankenphp:1-php8.4 sh /src/docker/frankenphp.sh
set -e
command -v gcc >/dev/null || { apt-get update -qq >/dev/null; apt-get install -y -qq $PHPIZE_DEPS curl >/dev/null 2>&1; }
rm -rf /build && mkdir /build && cd /src
cp *.c *.h config.m4 config.w32 /build/
cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>make.err || { cat make.err; exit 1; }
mkdir -p /app && cp /src/docker/frankenphp/app.php /app/
ini=/usr/local/etc/php/conf.d/nocopy.ini
run() { # $1 label, $2 extra ini, $3 worker flag
  rm -f /dev/shm/nocopy-fp.*
  printf 'extension=/build/modules/nocopy.so\nnocopy.shm_name=/nocopy-fp\nnocopy.shm_size=16M\n%s\n' "$2" > $ini
  frankenphp php-server --listen 127.0.0.1:8080 --root /app $3 >/tmp/fp.log 2>&1 & pid=$!
  for i in $(seq 50); do curl -sf 'http://127.0.0.1:8080/app.php?n=0' >/dev/null 2>&1 && break; sleep 0.2; done
  for i in $(seq 1 300); do curl -s "http://127.0.0.1:8080/app.php?n=$i" > /tmp/last; done &
  for i in $(seq 301 600); do curl -s "http://127.0.0.1:8080/app.php?n=$i" > /dev/null; done
  wait %2 2>/dev/null || true; sleep 0.5
  echo "$1: $(cat /tmp/last)"
  kill $pid; wait $pid 2>/dev/null || true
}
run "classic, auto      " "" ""
run "worker, auto       " "" "--worker /app/app.php"
run "worker, scope=1    " "nocopy.request_scope=1" "--worker /app/app.php"
