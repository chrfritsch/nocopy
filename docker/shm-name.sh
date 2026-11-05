#!/bin/sh
# nocopy.shm_name: the CLI and the web server (php-fpm, or FrankenPHP classic + worker) share one cache; it survives server restarts. Exit 1 on any failure.
#   docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 sh /src/docker/shm-name.sh
#   docker run --rm -v "$PWD":/src:ro dunglas/frankenphp:1-php8.4 sh /src/docker/shm-name.sh
command -v gcc >/dev/null || { apt-get update -qq >/dev/null; apt-get install -y -qq $PHPIZE_DEPS curl >/dev/null 2>&1; }
rm -rf /build && mkdir /build && cp /src/*.c /src/*.h /src/config.m4 /src/config.w32 /build/
(cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>/build/make.err) || { cat /build/make.err; exit 1; }
mkdir -p /app && cp /src/docker/shm-name/app.php /app/
printf 'extension=/build/modules/nocopy.so\napc.enable_cli=1\nnocopy.shm_name=/nocopy\nnocopy.shm_size=32M\n' > /usr/local/etc/php/conf.d/zz-nocopy.ini
rm -f /dev/shm/nocopy.*
bad=0
cli() { php -r "\$_GET = ['op' => '$1', 'k' => '$2']; require '/app/app.php';" 2>&1; }
# label, response, expected "from", expected zero_copy
check() { printf '%-16s %s\n' "$1" "$2"
  case $2 in *"\"hit\":true,\"from\":\"$3\""*"\"zero_copy\":$4"*) ;; *) echo "  ^ FAIL"; bad=1;; esac; }
# 3 CLI writers overwrite c0..c19 while the server reads c7 3000 times: every read must hit
churn() { web put c7 >/dev/null
  pids=; for w in 1 2 3; do (for i in $(seq 300); do cli put "c$((i % 20))" >/dev/null; done) & pids="$pids $!"; done
  m=$(for i in $(seq 3000); do web get c7; done | grep -vc '"hit":true')
  wait $pids
  echo "misses during churn: $m"; [ "$m" = 0 ] || bad=1; }
logcheck() { if grep -E 'segfault|SIGSEGV|exited on signal|panic|PHP (Warning|Fatal)|"level":"error"' "$1"; then bad=1; else echo "server log clean"; fi; }

if command -v php-fpm >/dev/null; then
  rm -f /etc/nginx/sites-enabled/default
  echo 'server { listen 8099; location / { fastcgi_pass 127.0.0.1:9001; include fastcgi_params; fastcgi_param SCRIPT_FILENAME /app/app.php; } }' > /etc/nginx/conf.d/t.conf
  nginx
  printf '[www]\nlisten = 127.0.0.1:9001\npm = static\npm.max_children = 4\n' > /usr/local/etc/php-fpm.d/zz-t.conf
  start() { php-fpm -F >>/tmp/fpm.log 2>&1 & fpm=$!; sleep 1; }
  stop() { kill $fpm; wait $fpm 2>/dev/null; }
  web() { curl -s "127.0.0.1:8099/?op=$1&k=$2"; }
  start
  check "cli put" "$(cli put a)" cli false
  check "fpm get" "$(web get a)" cli true
  check "fpm put" "$(web put b)" fpm-fcgi true
  check "cli get" "$(cli get b)" fpm-fcgi false
  kill -USR2 $fpm; sleep 1
  check "after reload" "$(web get a)" cli true
  stop; start
  check "after restart" "$(web get b)" fpm-fcgi true
  # another user cannot open it (mode 0600): warning, private cache
  su www-data -s /bin/sh -c "php -r 'var_dump(nocopy_get(\"a\"));'" 2>&1 | grep -q 'no permission' && echo "www-data cli     unavailable + warning" || { echo "www-data cli: FAIL"; bad=1; }
  churn
  logcheck /tmp/fpm.log
  stop
else
  run() { frankenphp php-server --listen 127.0.0.1:8080 --root /app $1 >>/tmp/fp.log 2>&1 & fp=$!
    for i in $(seq 50); do curl -sf '127.0.0.1:8080/app.php?op=get&k=x' >/dev/null 2>&1 && break; sleep 0.2; done; }
  web() { curl -s "127.0.0.1:8080/app.php?op=$1&k=$2"; }
  run ""
  check "cli put" "$(cli put a)" cli false
  check "classic get" "$(web get a)" cli true
  check "classic put" "$(web put b)" frankenphp true
  check "cli get" "$(cli get b)" frankenphp false
  kill $fp; wait $fp 2>/dev/null
  run "--worker /app/app.php"
  check "worker get" "$(web get a)" cli false
  check "cli put" "$(cli put c)" cli false
  check "worker get" "$(web get c)" cli false
  churn
  logcheck /tmp/fp.log
  kill $fp; wait $fp 2>/dev/null
fi
exit $bad
