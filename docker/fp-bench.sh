#!/bin/sh
# Inside dunglas/frankenphp: bench/sfapp (with vendor, mounted at /appsrc) in FrankenPHP worker mode, load from ab, median of 3.
# Same pages and OPcache settings as fpm-bench.sh; nocopy runs in copy mode here (kept copies, nocopy.local_size). Run via ./dev fpbench.
set -e
command -v ab >/dev/null || { apt-get update -qq >/dev/null; apt-get install -y -qq $PHPIZE_DEPS apache2-utils >/dev/null 2>&1; }
[ -f "$(php-config --extension-dir)/apcu.so" ] || (cd /tmp && curl -fsSLO https://pecl.php.net/get/apcu-5.1.28.tgz \
  && echo "ca9c1820810a168786f8048a4c3f8c9e3fd941407ad1553259fb2e30b5f057bf  apcu-5.1.28.tgz" | sha256sum -c - >/dev/null \
  && tar xzf apcu-5.1.28.tgz && cd apcu-5.1.28 && phpize && ./configure && make -j"$(nproc)" install) >/dev/null 2>&1 || { echo "apcu build failed"; exit 1; }
rm -rf /build && mkdir /build && cp -R /src/*.c /src/*.h /src/config.m4 /src/arena /src/third_party /build/
cd /build && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null 2>make.err || { cat make.err; exit 1; }
W=${WORKERS:-8}; N=${REQS:-20000}; C=${CONC:-8}
rm -rf /app && cp -r /appsrc /app && cp -r /src/bench/sfapp/src /src/bench/sfapp/public /app/ && mkdir -p /app/nocopy-bundle && cp -r /src/symfony/src /app/nocopy-bundle/
cat > /app/public/index.php <<'EOF'
<?php
use App\Kernel;
use Symfony\Component\HttpFoundation\Request;

require dirname(__DIR__).'/vendor/autoload.php';
$kernel = new Kernel('prod', false);
$handle = function () use ($kernel) {
    $request = Request::createFromGlobals();
    $response = $kernel->handle($request);
    $response->send();
    $kernel->terminate($request, $response);
};
while (frankenphp_handle_request($handle)) gc_collect_cycles();
EOF
cat > /app/Caddyfile <<EOF
{
	auto_https off
	frankenphp {
		num_threads $((W * 2))
		worker /app/public/index.php $W
	}
}
:8099 {
	root * /app/public
	php_server
}
EOF
run_cfg() { name=$1; raw=$2; shift 2
  printf '%s\n' "$@" "apc.shm_size=256M" "apc.entries_hint=20000" "memory_limit=1G" "opcache.enable=1" "opcache.validate_timestamps=0" \
    "opcache.memory_consumption=256" "opcache.max_accelerated_files=32531" "opcache.interned_strings_buffer=32" \
    "realpath_cache_size=4096K" "realpath_cache_ttl=600" > "$PHP_INI_DIR/conf.d/zz-bench.ini"
  rm -rf /app/var/cache/* /dev/shm/nocopy-bench.*
  RAW=$raw frankenphp run --config /app/Caddyfile >/tmp/fp.log 2>&1 & fp=$!
  for i in $(seq 100); do curl -sf 127.0.0.1:8099/hello >/dev/null 2>&1 && break; sleep 0.2; done
  curl -sf 127.0.0.1:8099/hello >/dev/null || { echo "frankenphp did not start ($name)"; tail -20 /tmp/fp.log; exit 1; }
  for u in hello 'products?page=3' product/77 validate; do curl -s -o /dev/null "127.0.0.1:8099/$u"; done
  ab -q -n 4000 -c "$C" "127.0.0.1:8099/products?page=3" >/dev/null 2>&1
  for u in hello 'products?page=3' product/77 validate; do
    for i in 1 2 3; do
      ab -q -n "$N" -c "$C" "127.0.0.1:8099/$u" 2>/dev/null | awk '/Requests per second/{r=$4} /Failed requests/{f=$3} /Non-2xx/{x=$3} END{print r, f+0, x+0}'
    done | sort -n | sed -n 2p | awk -v n="$name" -v u="$u" '{printf "%-24s %-18s %8.0f req/s  failed %s  non-2xx %s\n", n, u, $1, $2, $3}'
  done
  kill $fp; wait $fp 2>/dev/null || true; sleep 1
}
run_cfg "apcu / default pool" 0 "extension=apcu" "apc.serializer=default"
run_cfg "nocopy / default pool" 0 "extension=/build/modules/nocopy.so" "nocopy.shm_name=/nocopy-bench" "nocopy.shm_size=256M" "nocopy.entries_hint=20000"
run_cfg "apcu / raw pool" 1 "extension=apcu" "apc.serializer=default"
run_cfg "php files / opcache" files "extension=apcu" "apc.serializer=default"
run_cfg "nocopy copy / raw pool" 1 "extension=/build/modules/nocopy.so" "nocopy.shm_name=/nocopy-bench" "nocopy.shm_size=256M" "nocopy.entries_hint=20000"
run_cfg "nocopy, no kept copies" 1 "extension=/build/modules/nocopy.so" "nocopy.shm_name=/nocopy-bench" "nocopy.shm_size=256M" "nocopy.entries_hint=20000" "nocopy.local_size=0"
rm -f /dev/shm/nocopy-bench.*
