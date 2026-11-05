#!/bin/sh
# Inside the image: bench/sfapp (with vendor, mounted at /appsrc) behind nginx + php-fpm (static workers), load from ab, median of 3.
#   docker run --rm --shm-size=1g -v "$PWD":/src:ro -v <app with vendor>:/appsrc:ro nocopy-linux:8.5 /src/docker/fpm-bench.sh
/src/docker/test.sh >/dev/null 2>&1
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
W=${WORKERS:-8}; N=${REQS:-20000}; C=${CONC:-8}
rm -rf /app && cp -r /appsrc /app && cp -r /src/bench/sfapp/src /src/bench/sfapp/public /app/ && mkdir -p /app/nocopy-bundle && cp -r /src/symfony/src /app/nocopy-bundle/ && chown -R www-data /app/var 2>/dev/null; mkdir -p /app/var && chown -R www-data /app/var
rm -f /etc/nginx/sites-enabled/default
cat > /etc/nginx/conf.d/bench.conf <<'EOF'
server { listen 8099; access_log off; location / { fastcgi_pass 127.0.0.1:9001; include fastcgi_params; fastcgi_param SCRIPT_FILENAME /app/public/index.php; } }
EOF
nginx
run_cfg() { name=$1; raw=$2; shift 2
  printf '[www]\nlisten = 127.0.0.1:9001\npm = static\npm.max_children = %s\nclear_env = no\nenv[RAW] = %s\n' "$W" "$raw" > /usr/local/etc/php-fpm.d/zz-bench.conf
  printf '%s\n' "$@" "apc.shm_size=256M" "apc.entries_hint=20000" "memory_limit=1G" "opcache.enable=1" "opcache.validate_timestamps=0" > /usr/local/etc/php/conf.d/zz-bench.ini
  rm -rf /app/var/cache/* /dev/shm/nocopy-bench.*
  php-fpm -F >/dev/null 2>&1 & fpm=$!
  sleep 2
  kill -0 $fpm 2>/dev/null || { echo "php-fpm did not start ($name)"; php-fpm -t; exit 1; }
  for u in hello 'products?page=3' product/77 validate; do curl -s -o /dev/null "127.0.0.1:8099/$u"; done
  ab -q -n 4000 -c "$C" "127.0.0.1:8099/products?page=3" >/dev/null 2>&1
  for u in hello 'products?page=3' product/77 validate; do
    for i in 1 2 3; do
      ab -q -n "$N" -c "$C" "127.0.0.1:8099/$u" 2>/dev/null | awk '/Requests per second/{r=$4} /Failed requests/{f=$3} /Non-2xx/{x=$3} END{print r, f+0, x+0}'
    done | sort -n | sed -n 2p | awk -v n="$name" -v u="$u" '{printf "%-24s %-18s %8.0f req/s  failed %s  non-2xx %s\n", n, u, $1, $2, $3}'
  done
  kill $fpm; wait $fpm 2>/dev/null; sleep 1
}
run_cfg "apcu / default pool" 0 "extension=$APCU" "apc.serializer=default"
run_cfg "nocopy / default pool" 0 "extension=/build/modules/nocopy.so" "nocopy.shm_name=/nocopy-bench" "nocopy.apcu_compat=1"
run_cfg "apcu / raw pool" 1 "extension=$APCU" "apc.serializer=default"
run_cfg "nocopy copy / raw pool" 1 "extension=/build/modules/nocopy.so" "nocopy.shm_name=/nocopy-bench" "nocopy.apcu_compat=1" "nocopy.request_scope=0"
run_cfg "nocopy zero / raw pool" 1 "extension=/build/modules/nocopy.so" "nocopy.shm_name=/nocopy-bench" "nocopy.apcu_compat=1"
