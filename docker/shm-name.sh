#!/bin/sh
# nocopy.shm_name: the CLI and the web server (php-fpm, or FrankenPHP classic + worker) share one cache; it survives server restarts.
# Then with nocopy.valkey: one listener for the host, outliving server restarts. Exit 1 on any failure.
#   docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 sh /src/docker/shm-name.sh
#   docker run --rm -v "$PWD":/src:ro dunglas/frankenphp:1-php8.4 sh /src/docker/shm-name.sh
command -v gcc >/dev/null && command -v valkey-server >/dev/null || { apt-get update -qq >/dev/null; apt-get install -y -qq $PHPIZE_DEPS curl redis-server >/dev/null 2>&1; }
rm -rf /build && mkdir /build && cp -R /src/*.c /src/*.h /src/config.m4 /src/arena /src/third_party /build/
(cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>/build/make.err) || { cat /build/make.err; exit 1; }
mkdir -p /app && cp /src/docker/shm-name/app.php /app/
ini() { printf 'extension=/build/modules/nocopy.so\nnocopy.shm_name=/nocopy\nnocopy.shm_size=32M\n%s\n' "$1" > /usr/local/etc/php/conf.d/zz-nocopy.ini; }
ini ""
rm -f /dev/shm/nocopy.*
bad=0
as() { if [ -n "$AS" ]; then runuser -u "$AS" -- "$@"; else "$@"; fi; } # fpm: the workers' user, not root
cli() { as php -r "\$_GET = ['op' => '$1', 'k' => '$2']; require '/app/app.php';" 2>&1; }
# label, response, expected "from", expected zero_copy
check() { printf '%-16s %s\n' "$1" "$2"
  case $2 in *"\"hit\":true,\"from\":\"$3\""*"\"zero_copy\":$4"*) ;; *) echo "  ^ FAIL"; bad=1;; esac; }
# 3 CLI writers overwrite c0..c19 while the server reads c7 3000 times: every read must hit
churn() { web put c7 >/dev/null
  pids=; for w in 1 2 3; do (for i in $(seq 300); do cli put "c$((i % 20))" >/dev/null; done) & pids="$pids $!"; done
  m=$(for i in $(seq 3000); do web get c7; done | grep -vc '"hit":true')
  wait $pids
  echo "misses during churn: $m"; [ "$m" = 0 ] || bad=1; }
vk=$(command -v valkey-cli || command -v redis-cli)
listeners() { $vk client list | grep -c name=nocopy-listener; }
lpid() { as php -r 'echo nocopy_stats()["valkey_listener"];'; }
# label, expected count
count() { n=$(listeners); printf '%-16s %s\n' "$1" "$n listener(s)"; [ "$n" = "$2" ] || { echo "  ^ FAIL"; bad=1; }; }
# with Valkey: server hits from many workers and the CLI, one listener; it outlives a server restart, the copy stays warm
valkey_pass() { start_vk=$1 stop_vk=$2
  ini 'nocopy.valkey=127.0.0.1:6379'; rm -f /dev/shm/nocopy.*; $vk flushall >/dev/null
  $start_vk
  check "vk cli put" "$(cli put a)" cli false
  for i in $(seq 20); do web get a >/dev/null; done
  check "vk server get" "$(web get a)" cli "$3"
  count "one per host" 1
  p1=$(lpid); $stop_vk; $start_vk
  [ "$(lpid)" = "$p1" ] && echo "listener kept    over the restart" || { echo "listener kept: FAIL"; bad=1; }
  $vk config resetstat >/dev/null
  check "vk warm get" "$(web get a)" cli "$3"
  $vk info commandstats | grep -q cmdstat_get && { echo "  ^ FAIL: went to Valkey"; bad=1; }
  kill -9 "$p1"; $vk set nocopy:a 1 >/dev/null; sleep 1.2
  case $(web get a) in *'"hit":true,"from":null'*) echo "after kill -9    missed change seen";; *) echo "after kill -9: FAIL"; bad=1;; esac
  count "restarted" 1
  churn
  $stop_vk; }

logcheck() { if grep -E 'segfault|SIGSEGV|exited on signal|panic|PHP (Warning|Fatal)|"level":"error"' "$1"; then bad=1; else echo "server log clean"; fi; }

if command -v apache2ctl >/dev/null; then
  # mod_php: the parent (root) runs MINIT, the children are www-data
  cp /app/app.php /var/www/html/
  AS=www-data
  start() { apache2ctl start >>/tmp/apache.log 2>&1; for i in $(seq 50); do curl -sf '127.0.0.1/app.php?op=get&k=x' >/dev/null 2>&1 && break; sleep 0.2; done; }
  stop() { apache2ctl stop >>/tmp/apache.log 2>&1; while pgrep -x apache2 >/dev/null; do sleep 0.1; done; }
  web() { curl -s "127.0.0.1/app.php?op=$1&k=$2"; }
  start
  check "cli put" "$(cli put a)" cli false
  check "apache get" "$(web get a)" cli true
  check "apache put" "$(web put b)" apache2handler true
  check "cli get" "$(cli get b)" apache2handler false
  apache2ctl graceful; sleep 1
  check "after graceful" "$(web get a)" cli true
  stop; start
  check "after restart" "$(web get b)" apache2handler true
  [ "$(stat -c %U /dev/shm/nocopy.*)" = www-data ] && echo "owner            www-data" || { echo "owner: FAIL"; ls -l /dev/shm; bad=1; }
  churn
  stop
  logcheck /tmp/apache.log
elif command -v php-fpm >/dev/null; then
  rm -f /etc/nginx/sites-enabled/default /dev/shm/nocopy-b.*
  echo 'server { listen 8099; location / { fastcgi_pass 127.0.0.1:9001; include fastcgi_params; fastcgi_param SCRIPT_FILENAME /app/app.php; } }' > /etc/nginx/conf.d/t.conf
  echo 'server { listen 8098; location / { fastcgi_pass 127.0.0.1:9002; include fastcgi_params; fastcgi_param SCRIPT_FILENAME /app/app.php; } }' >> /etc/nginx/conf.d/t.conf
  nginx
  # www: the image's pool (www-data); b: another user with its own cache
  printf '[www]\nlisten = 127.0.0.1:9001\npm = static\npm.max_children = 4\n[b]\nuser = nobody\ngroup = nogroup\nlisten = 127.0.0.1:9002\npm = static\npm.max_children = 2\nphp_admin_value[nocopy.shm_name] = /nocopy-b\n' > /usr/local/etc/php-fpm.d/zz-t.conf
  AS=www-data
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
  # the master (root) never maps it: the workers create it as their user, and root does not attach
  [ "$(stat -c %U /dev/shm/nocopy.*)" = www-data ] && echo "owner            www-data" || { echo "owner: FAIL"; ls -l /dev/shm; bad=1; }
  php -r 'var_dump(nocopy_get("a"));' 2>&1 | grep -q 'owned by another user' && echo "root cli         unavailable + warning" || { echo "root cli: FAIL"; bad=1; }
  # pool b (nobody, php_admin_value name): its own cache, not www's
  case $(curl -s '127.0.0.1:8098/?op=get&k=b') in *'"hit":false'*) echo "pool b           own cache";; *) echo "pool b: FAIL"; bad=1;; esac
  [ "$(stat -c %U /dev/shm/nocopy-b.*)" = nobody ] && echo "pool b owner     nobody" || { echo "pool b owner: FAIL"; bad=1; }
  churn
  stop
  if [ -n "$vk" ]; then (valkey-server --daemonize yes --save '' >/dev/null 2>&1 || redis-server --daemonize yes --save '' >/dev/null); sleep 0.3; valkey_pass start stop true; fi
  grep -i 'address already in use' /tmp/fpm.log && bad=1
  logcheck /tmp/fpm.log
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
  kill $fp; wait $fp 2>/dev/null
  redis-server --daemonize yes --save '' >/dev/null; sleep 0.3
  fp_start() { run ""; }
  fp_stop() { kill $fp; wait $fp 2>/dev/null; }
  valkey_pass fp_start fp_stop true
  grep -i 'address already in use' /tmp/fp.log && bad=1
  logcheck /tmp/fp.log
fi
exit $bad
