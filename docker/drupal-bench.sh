#!/bin/sh
# Inside nocopy-drupal: Drupal 11 on nginx + php-fpm + MariaDB, nine cache configs, anonymous / logged-in / editor / cold start.
#   docker run --rm --shm-size=1g --cap-add=SYS_PTRACE -v "$PWD":/src:ro -v nocopy-drupal-bench:/site -v "$PWD/bench/results-drupal":/out nocopy-drupal /src/docker/drupal-bench.sh
# MEM_ONLY=1: per-worker memory after warm-up and a fixed load only (mem.tsv), no throughput runs.
# PERF_ONLY=1 (needs --privileged): after warm-up, perf over the whole container while ab loads a node page, anonymous and logged in:
#   perf-<cfg>-<anon|auth>.dso (samples by process, library) and .sym (php-fpm's top symbols). PERF_N=4000 requests each.
# TRACE_ONLY=1: after warm-up, strace on every php-fpm worker over 200 node page requests, anonymous and logged in:
#   trace-<cfg>-<anon|auth>.sum (syscalls per request) and .paths (file syscalls per request by path, most first).
# Env: WORKERS=8 CONC=8 REQS=5000 (anonymous) REQS_AUTH=1000 (logged-in) EDITOR_T=40 (s of reader load during the editor loop) CONFIGS="stock stock-igbinary apcu-all nocopy-fast nocopy-all-dbtags nocopy-all redis redis2 nocopy-valkey"
#   (--shm-size: every nocopy config keeps its arena in /dev/shm)
W=${WORKERS:-8}; C=${CONC:-8}; N=${REQS:-5000}; NA=${REQS_AUTH:-1000}; ET=${EDITOR_T:-40}
CONFIGS=${CONFIGS:-"stock stock-igbinary apcu-all nocopy-fast nocopy-all-dbtags nocopy-all redis redis2 nocopy-valkey"}
B=http://127.0.0.1:8099; B2=http://127.0.0.1:8098; D=/site/d; DRUSH="php -d memory_limit=2G $D/vendor/drush/drush/drush.php -r $D/web"
APCU=$(find /usr/local/lib/php/extensions -name apcu.so)
export COMPOSER_ALLOW_SUPERUSER=1 COMPOSER_NO_INTERACTION=1

/src/docker/test.sh >/dev/null 2>&1 || { echo "nocopy build failed"; exit 1; }
mkdir -p /out

# MariaDB, data kept in the /site volume
printf '[mysqld]\ndatadir=/site/mysql\ninnodb_buffer_pool_size=512M\nmax_connections=200\n' > /etc/mysql/mariadb.conf.d/99-bench.cnf
[ -d /site/mysql/mysql ] || mariadb-install-db --datadir=/site/mysql --user=mysql >/dev/null
chown -R mysql /site/mysql
mysqld_safe >/dev/null 2>&1 &
for i in $(seq 30); do mariadb -e 'select 1' >/dev/null 2>&1 && break; sleep 1; done
mariadb -e "CREATE DATABASE IF NOT EXISTS drupal; CREATE USER IF NOT EXISTS drupal@'127.0.0.1' IDENTIFIED BY 'drupal'; GRANT ALL ON drupal.* TO drupal@'127.0.0.1';"

valkey-cli ping >/dev/null 2>&1 || valkey-server --daemonize yes --save '' --appendonly no >/dev/null

# Site, once
if [ ! -f /site/installed ]; then
  composer create-project -q drupal/recommended-project:^11 $D && (cd $D && composer require -q drush/drush drupal/devel) || exit 1
  $DRUSH si standard --db-url=mysql://drupal:drupal@127.0.0.1/drupal --account-pass=admin -y >/dev/null 2>&1 || { echo "site install failed"; exit 1; }
  chmod u+w $D/web/sites/default $D/web/sites/default/settings.php
  echo 'include $app_root . "/" . $site_path . "/settings.bench.php";' >> $D/web/sites/default/settings.php
  touch /site/installed
fi
cp /src/bench/drupal/settings.stock.php $D/web/sites/default/settings.bench.php   # setup and drush run on stock
rm -f /usr/local/etc/php/conf.d/zz-bench.ini
if [ "$($DRUSH sqlq 'select count(*) from node')" -lt 1000 ]; then
  # 11.4's standard profile and recipe create no content types: those come from their own recipes
  for r in standard article_content_type page_content_type article_tags article_comment; do
    $DRUSH recipe core/recipes/$r 2>&1 | tail -1
  done
  $DRUSH en devel_generate -y >/dev/null 2>&1
  $DRUSH devel-generate:users 20 2>&1 | tail -1
  $DRUSH devel-generate:terms 50 --bundles=tags 2>&1 | tail -1
  $DRUSH devel-generate:content 2000 --kill --bundles=article,page 2>&1 | tail -1
  $DRUSH devel-generate:menus 2 30 2>&1 | tail -1
fi
[ -d /site/redis1 ] || { (cd $D && composer require -q drupal/redis:1.11.0) && cp -r $D/web/modules/contrib/redis /site/redis1; } || exit 1
# drupal/redis 2.x (alpha, so not what composer picks): swapped in for the redis2 config only
[ -d /site/redis2 ] || { mkdir -p /tmp/r2 && curl -sL https://ftp.drupal.org/files/projects/redis-2.0.0-alpha2.tar.gz | tar xz -C /tmp/r2 && mv /tmp/r2/redis /site/redis2; } || exit 1
$DRUSH en redis -y >/dev/null 2>&1   # its services; the bins only use it in the redis config
nodes=$($DRUSH sqlq 'select count(*) from node'); echo "nodes: $nodes"
[ "$nodes" -ge 1000 ] || { echo "content generation failed"; exit 1; }
rm -rf $D/web/modules/contrib/nocopy && mkdir -p $D/web/modules/contrib && cp -r /src/drupal $D/web/modules/contrib/nocopy
cp /src/bench/drupal/settings.*.php /src/bench/drupal/*.services.yml /src/bench/drupal/IgbinarySerialize.php $D/web/sites/default/
$DRUSH pmu nocopy -y >/dev/null 2>&1; $DRUSH en nocopy -y >/dev/null 2>&1   # reinstall: schema changes
echo '<?php echo function_exists("nocopy_stats") ? nocopy_stats()["used"] : 0;' > $D/web/nocopy-used.php
# serializer probe: 1000 equal strings in an object, ~115 KB with serialize(), ~5 KB with igbinary
# with Valkey the listener drops the writer's own fresh copy: refetch until it stays local
echo '<?php $o = new ArrayObject(array_fill(0, 1000, str_repeat("x", 100))); if (function_exists("nocopy_put")) { $k = "probe" . mt_rand(); $u = nocopy_stats()["used"]; nocopy_put($k, $o, 0); $d = nocopy_stats()["used"] - $u; for ($i = 0; !$d && $i < 200; $i++, usleep(1000)) { $u = nocopy_stats()["used"]; nocopy_get($k); $d = nocopy_stats()["used"] - $u; } echo $d; exit; } apcu_store("probe", $o); foreach (new APCUIterator("/^probe$/") as $e) echo $e["mem_size"];' > $D/web/serializer-probe.php
chown -R www-data $D/web/sites/default/files
NIDS=$($DRUSH sqlq "select nid from node_field_data where type='article' order by nid limit 25" | tr '\n' ' ')
NODE=$($DRUSH sqlq "select nid from node_field_data where type='article' order by nid limit 1 offset 40")
TERM=$($DRUSH sqlq "select tid from taxonomy_term_field_data order by tid limit 1 offset 2")
URLS="/ /node/$NODE /taxonomy/term/$TERM"

rm -f /etc/nginx/sites-enabled/default
cat > /etc/nginx/conf.d/drupal.conf <<'NGX'
server { listen 8099; root /site/d/web; access_log off;
  location / { try_files $uri /index.php?$query_string; }
  location ~ \.php$ { fastcgi_pass 127.0.0.1:9001; include fastcgi_params; fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name; }
  location = /fpm-status { fastcgi_pass 127.0.0.1:9001; include fastcgi_params; fastcgi_param SCRIPT_NAME /fpm-status; fastcgi_param SCRIPT_FILENAME /fpm-status; } }
server { listen 8098; root /site/d/web; access_log off;
  location / { try_files $uri /index.php?$query_string; }
  location ~ \.php$ { fastcgi_pass 127.0.0.1:9002; include fastcgi_params; fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name; } }
NGX
nginx

questions() { mariadb -N -e "SHOW GLOBAL STATUS LIKE 'Questions'" | awk '{print $2}'; }
fpm_start() { php-fpm -D; for i in $(seq 20); do curl -s -o /dev/null "$B/nocopy-used.php" && return; sleep 0.5; done; }
fpm_stop() { pkill -QUIT php-fpm; while pgrep php-fpm >/dev/null; do sleep 0.2; done; }
# a second web server: its own php-fpm master, so its own nocopy memory, on the same database and Valkey
fpmb_start() {
  printf '[global]\npid=/tmp/fpm-b.pid\nerror_log=/tmp/fpm-b.log\n[www]\nuser=www-data\ngroup=www-data\nlisten=127.0.0.1:9002\npm=static\npm.max_children=%s\naccess.log=/dev/null\n' "$W" > /tmp/fpm-b.conf
  php-fpm -D -y /tmp/fpm-b.conf; for i in $(seq 20); do curl -s -o /dev/null "$B2/nocopy-used.php" && return; sleep 0.5; done; }
login() { curl -s -c /tmp/ck -o /dev/null -L "$($DRUSH uli --uri=$B --no-browser 2>/dev/null)"; SID=$(awk '/SESS/{print $6"="$7}' /tmp/ck); }
# median of 3 ab runs: req/s p50 p99 failed; ARGS before the URL
ab3() { n=$1; shift
  for i in 1 2 3; do ab -q -l -n "$n" -c "$C" "$@" 2>/dev/null | tee -a /out/ab-raw.txt | awk '/Requests per second/{r=$4} /^ +50%/{p=$2} /^ +99%/{q=$2} /Failed requests/{f=$3} /Non-2xx/{x=$3} END{print r, p, q, f+x}'; done | sort -n | sed -n 2p; }

for cfg in $CONFIGS; do
  case $cfg in
    stock) EXT="extension=$APCU"; SET=stock ;;
    stock-igbinary) EXT="extension=$APCU
extension=igbinary
apc.serializer=igbinary"; SET=stock ;;
    apcu-all) EXT="extension=$APCU
extension=igbinary
apc.serializer=igbinary"; SET=apcu ;;
    nocopy-fast) EXT="extension=/build/modules/nocopy.so
nocopy.shm_name=/nocopy-bench
extension=igbinary
nocopy.serializer=igbinary"; SET=fast ;;
    nocopy-all-dbtags) EXT="extension=/build/modules/nocopy.so
extension=igbinary
nocopy.serializer=igbinary
nocopy.shm_name=/nocopy-bench"; SET=alldbtags ;;
    nocopy-all) EXT="extension=/build/modules/nocopy.so
extension=igbinary
nocopy.serializer=igbinary
nocopy.shm_name=/nocopy-bench"; SET=all ;;
    redis|redis2) EXT="extension=$APCU
extension=igbinary
apc.serializer=igbinary
extension=redis"; SET=redis ;;
    nocopy-valkey) EXT="extension=/build/modules/nocopy.so
extension=igbinary
nocopy.serializer=igbinary
nocopy.shm_name=/nocopy-bench
nocopy.valkey=127.0.0.1:6379"; SET=all ;;
  esac
  echo "== $cfg"
  rm -rf $D/web/modules/contrib/redis && cp -r /site/redis$([ $cfg = redis2 ] && echo 2 || echo 1) $D/web/modules/contrib/redis
  fpm_stop 2>/dev/null
  printf '[www]\nlisten = 127.0.0.1:9001\npm = static\npm.max_children = %s\naccess.log = /dev/null\npm.status_path = /fpm-status\n' "$W" > /usr/local/etc/php-fpm.d/zz-bench.conf
  printf '%s\n' "$EXT" "apc.shm_size=256M" "apc.entries_hint=50000" "memory_limit=1G" "opcache.enable=1" "opcache.validate_timestamps=0" "opcache.memory_consumption=256" > /usr/local/etc/php/conf.d/zz-bench.ini
  cp $D/web/sites/default/settings.$SET.php $D/web/sites/default/settings.bench.php
  valkey-cli flushall >/dev/null; rm -f /dev/shm/nocopy-bench.*
  $DRUSH cr >/dev/null 2>&1   # the database bins; shared memory starts empty with php-fpm
  fpm_start; login
  for u in $URLS; do for i in 1 2 3; do curl -s -o /dev/null "$B$u"; curl -s -o /dev/null -b /tmp/ck "$B$u"; done; done
  ab -q -n 500 -c "$C" -C "$SID" "$B/node/$NODE" >/dev/null 2>&1; ab -q -n 2000 -c "$C" "$B/" >/dev/null 2>&1

  # per worker, MB: RSS, PSS (shared pages split among their users), private (USS); PHP peak of its last request; Valkey's RSS
  for u in $URLS; do ab -q -n 400 -c "$C" "$B$u" >/dev/null 2>&1; ab -q -n 400 -c "$C" -C "$SID" "$B$u" >/dev/null 2>&1; done
  m=$(pgrep -f 'php-fpm: master' | head -1)
  for p in $(pgrep -P "$m"); do awk '/^Rss/{r=$2} /^Pss:/{s=$2} /^Private_(Clean|Dirty)/{u+=$2} END{print r, s, u}' /proc/$p/smaps_rollup; done > /tmp/mem.txt
  peak=$(curl -s "$B/fpm-status?full&json" | php -r '$p = json_decode(stream_get_contents(STDIN), true)["processes"]; echo round(array_sum(array_column($p, "last request memory")) / count($p) / 1048576, 1);')
  vk=$(valkey-cli info memory | awk -F: '/^used_memory_rss:/{printf "%.1f", $2 / 1048576}')
  awk -v c=$cfg -v k="$peak" -v v="$vk" '{r+=$1; s+=$2; u+=$3; n++} END{printf "%s\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\n", c, n, r/n/1024, s/n/1024, u/n/1024, s/1024, k, v}' /tmp/mem.txt > /out/mem.tsv.$cfg
  cat /out/mem.tsv.$cfg
  [ -n "$MEM_ONLY" ] && continue
  if [ -n "$TRACE_ONLY" ]; then
    P=$(pgrep -P "$m" | sed 's/^/-p /' | tr '\n' ' ')
    for k in anon auth; do
      ck=; [ $k = auth ] && ck="-C $SID"
      for mode in sum paths; do
        if [ $mode = sum ]; then strace -qq -c -o /tmp/st $P & else strace -qq -e trace=%file -o /tmp/st $P & fi
        s=$!; sleep 1
        ab -q -n 200 -c "$C" $ck "$B/node/$NODE" >/dev/null 2>&1
        kill -INT $s; wait $s 2>/dev/null
        if [ $mode = sum ]; then
          awk 'NR > 2 && $1 ~ /^[0-9.]+$/ && $NF != "total" {printf "%8.1f %s\n", $4 / 200, $NF}' /tmp/st | sort -rn > /out/trace-$cfg-$k.sum
        else
          sed -n 's/^[0-9 ]*\([a-z0-9_]*\)(\(AT_FDCWD, \)\{0,1\}"\([^"]*\)".*= \(-1 [A-Z]*\)\{0,1\}.*/\1 \3 \4/p' /tmp/st | sort | uniq -c | sort -rn | awk '{printf "%7.1f %s %s %s\n", $1 / 200, $2, $3, $4}' | head -40 > /out/trace-$cfg-$k.paths
        fi
      done
      echo "  $k: $(awk '{s += $1} END {printf "%.0f", s}' /out/trace-$cfg-$k.sum) syscalls/request"
    done
    continue
  fi
  if [ -n "$PERF_ONLY" ]; then
    echo -1 > /proc/sys/kernel/perf_event_paranoid
    for k in anon auth; do
      ck=; [ $k = auth ] && ck="-C $SID"
      perf record -q -a -F 999 -o /tmp/p.data -- ab -q -n "${PERF_N:-4000}" -c "$C" $ck "$B/node/$NODE" 2>/dev/null | awk -v k=$k '/Requests per second/{print "  " k ": " $4 " req/s"}'
      perf report -i /tmp/p.data --no-children --sort comm,dso --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -15 > /out/perf-$cfg-$k.dso
      perf report -i /tmp/p.data --no-children --comm php-fpm --sort dso,sym --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -60 > /out/perf-$cfg-$k.sym
    done
    continue
  fi

  out=/out/$cfg.tsv; printf 'traffic\turl\treq_s\tp50_ms\tp99_ms\tfailed\tqueries_per_req\n' > $out
  for u in $URLS; do
    q0=$(questions); r=$(ab3 "$N" "$B$u"); q1=$(questions)
    echo "anonymous $u $r $(awk -v a=$q0 -v b=$q1 -v n=$N 'BEGIN{printf "%.1f", (b-a)/(3*n)}')" | tr ' ' '\t' >> $out
    q0=$(questions); r=$(ab3 "$NA" -C "$SID" "$B$u"); q1=$(questions)
    echo "logged-in $u $r $(awk -v a=$q0 -v b=$q1 -v n=$NA 'BEGIN{printf "%.1f", (b-a)/(3*n)}')" | tr ' ' '\t' >> $out
  done
  cat $out

  # editor loop under reader load
  ab -l -t "$ET" -n 10000000 -c 4 -C "$SID" "$B/node/$NODE" > /tmp/reader.txt 2>&1 &
  ab_pid=$!
  /src/bench/drupal/editor.sh "$B" /tmp/ck /tmp/editor.txt "$NIDS $NIDS" || echo "EDITOR SAVES FAILED ($cfg)"
  wait $ab_pid
  reader=$(awk '/Requests per second/{print $4}' /tmp/reader.txt)
  for s in edit save view list add; do
    awk -v s=$s '$1==s{print $2*1000}' /tmp/editor.txt | sort -n | awk -v s=$s -v c=$cfg -v r="$reader" '{v[NR]=$1} END{printf "%s\t%s\t%.1f\t%s\n", c, s, v[int((NR+1)/2)], r}'
  done >> /out/editor.tsv.$cfg
  cat /out/editor.tsv.$cfg

  # a node saved by Drush must show up in php-fpm's pages
  title="cli-$cfg-$(date +%s)"
  $DRUSH php:eval "\$n = \\Drupal\\node\\Entity\\Node::load($NODE); \$n->setTitle('$title'); \$n->save();" >/dev/null 2>&1
  printf '%s\t%s\t%s\n' "$cfg" "$(curl -s "$B/node/$NODE" | grep -c "$title")" "$(curl -s -b /tmp/ck "$B/node/$NODE" | grep -c "$title")" > /out/cli.tsv.$cfg
  cat /out/cli.tsv.$cfg

  # shared configs: a second server joins (first requests on an empty local memory), then a web save on A must show on B
  case $cfg in redis|redis2|nocopy-valkey)
    fpmb_start
    for i in $(seq 30); do curl -s -o /dev/null -b /tmp/ck -w '%{time_total}\n' "$B2/"; done > /tmp/b.txt
    /src/bench/drupal/editor.sh "$B" /tmp/ck /tmp/ed1.txt "$NODE" >/dev/null 2>&1 || echo "SAVE ON A FAILED ($cfg)"
    ta=$(curl -s -b /tmp/ck "$B/node/$NODE" | grep -o '<title>[^<]*'); tb=$(curl -s -b /tmp/ck "$B2/node/$NODE" | grep -o '<title>[^<]*')
    an=$(curl -s "$B2/node/$NODE" | grep -o '<title>[^<]*')
    first=$(head -1 /tmp/b.txt); med=$(tail -n +6 /tmp/b.txt | sort -n | sed -n 13p)
    printf '%s\t%.1f\t%.1f\t%s\t%s\n' $cfg "$(awk -v x=$first 'BEGIN{print x*1000}')" "$(awk -v x=$med 'BEGIN{print x*1000}')" \
      "$([ -n "$ta" ] && [ "$ta" = "$tb" ] && echo 1 || echo 0)" "$([ -n "$ta" ] && [ "$ta" = "$an" ] && echo 1 || echo 0)" > /out/server2.tsv.$cfg
    cat /out/server2.tsv.$cfg
    kill -QUIT "$(cat /tmp/fpm-b.pid)"; while [ -f /tmp/fpm-b.pid ] && pgrep -F /tmp/fpm-b.pid >/dev/null 2>&1; do sleep 0.2; done ;;
  esac

  used=$(curl -s "$B/nocopy-used.php")
  printf '%s\t%s\n' "$cfg" "$(curl -s "$B/serializer-probe.php")" >> /out/serializer.tsv.$cfg
  # cold start: 100 sequential logged-in requests to / right after a php-fpm restart
  p50=$(awk -F'\t' '$1=="logged-in" && $2=="/"{print $4}' $out)
  fpm_stop; fpm_start
  for i in $(seq 100); do curl -s -o /dev/null -b /tmp/ck -w '%{time_total}\n' "$B/"; done > /tmp/cold.txt
  awk -v c=$cfg -v p=$p50 -v u="$used" '{t[NR]=$1*1000} END{
    for (i=1; i<=NR-9; i++) { ok=1; for (j=i; j<i+10; j++) if (t[j] > 1.5*p) ok=0; if (ok) break }
    printf "%s\t%.1f\t%d\t%s\t%s\n", c, t[1], i, p, u }' /tmp/cold.txt > /out/coldstart.tsv.$cfg
  cat /out/coldstart.tsv.$cfg
done
fpm_stop
cp /src/bench/drupal/settings.stock.php $D/web/sites/default/settings.bench.php
rm -f /usr/local/etc/php/conf.d/zz-bench.ini
{ printf 'config\tstep\tp50_ms\treader_req_s\n'; cat /out/editor.tsv.* 2>/dev/null; } > /out/editor.tsv
{ printf 'config\tfirst_ms\tsettled_at_request\twarm_p50_ms\tnocopy_used_bytes\n'; cat /out/coldstart.tsv.* 2>/dev/null; } > /out/coldstart.tsv
{ printf 'config\tfirst_ms\tp50_ms_requests_6_30\tlogged_in_sees_save_on_a\tanonymous_sees_save_on_a\n'; cat /out/server2.tsv.* 2>/dev/null; } > /out/server2.tsv
{ printf 'config\tanonymous_sees_cli_save\tlogged_in_sees_cli_save\n'; cat /out/cli.tsv.* 2>/dev/null; } > /out/cli.tsv
{ printf 'config\tprobe_bytes\n'; cat /out/serializer.tsv.* 2>/dev/null; } > /out/serializer.tsv
{ printf 'config\tworkers\trss_mb\tpss_mb\tprivate_mb\tpss_total_mb\tphp_peak_mb\tvalkey_rss_mb\n'; cat /out/mem.tsv.* 2>/dev/null; } > /out/mem.tsv
rm -f /out/mem.tsv.* /out/editor.tsv.* /out/coldstart.tsv.* /out/serializer.tsv.* /out/cli.tsv.* /out/server2.tsv.*
mariadb-admin shutdown
