#!/bin/sh
# Same hitratio.php workloads against nocopy, APCu and RESP (allkeys-lru, allkeys-lfu), each given the same memory. In nocopy-drupal.
/src/docker/build.sh >/dev/null || exit 1
X=$(php-config --extension-dir)
NC="-d extension=/build/modules/nocopy.so -d memory_limit=-1"
valkey-server --port 6379 --save '' --appendonly no --daemonize yes --logfile /tmp/vk.log; sleep 1
i=0
for size in 32M 64M; do
  mb=${size%M}
  for wl in "zipf 1.5 0 0" "zipf 1.5 0.5 0" "zipf 3 0 0" "loop 1.1 0 0" "loop 1.5 0 0" "zipf 1.5 0 0.2"; do
    i=$((i + 1))
    printf "%s %-16s nocopy    " $size "$wl"; php -n $NC -d nocopy.shm_name=/hr$i -d nocopy.shm_size=$size /src/bench/hitratio.php $wl
    printf "%s %-16s apcu      " $size "$wl"; HR_BACKEND=apcu php -n $NC -d nocopy.shm_name=/hr$i -d nocopy.shm_size=$size -d extension=$X/apcu.so -d apc.enable_cli=1 -d apc.shm_size=$size /src/bench/hitratio.php $wl
    for pol in allkeys-lru allkeys-lfu; do
      valkey-cli config set maxmemory ${mb}mb >/dev/null; valkey-cli config set maxmemory-policy $pol >/dev/null; valkey-cli config resetstat >/dev/null
      printf "%s %-16s %-9s " $size "$wl" ${pol#allkeys-}; HR_BACKEND=redis php -n $NC -d nocopy.shm_name=/hr$i -d nocopy.shm_size=$size -d extension=$X/redis.so /src/bench/hitratio.php $wl
    done
  done
done
valkey-server --version
