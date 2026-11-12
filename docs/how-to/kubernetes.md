# Run nocopy on Kubernetes

Goal: php-fpm pods, their CLI processes and CronJobs share one cache. Install the extension in your image first
([Install on a server](install.md#with-pie)).

## Pick the layout

| What shares the cache | Needs |
|---|---|
| php-fpm and `kubectl exec … drush` in **one container** | `nocopy.shm_name` and a `/dev/shm` volume |
| Containers of **one pod** (php-fpm plus a CLI or worker sidecar) | the same volume mounted at `/dev/shm` in each, the same image, the same uid |
| **Several pods**: replicas, separate deployments, CronJobs | `nocopy.resp` as well: RESP is the shared cache, each pod keeps local copies ([RESP](resp.md)) |

Shared memory does not cross pods: anything that writes in one pod and reads in another needs RESP. A Drupal or Symfony
site with more than one replica, or with cron in its own pod, is in the last row.

## 1. Give the pod a `/dev/shm`

Containers get a 64 MB `/dev/shm` by default, and nocopy reserves its whole `nocopy.shm_size` there when the cache is
created. Mount a memory-backed `emptyDir` instead:

```yaml
spec:
  containers:
    - name: php
      volumeMounts:
        - { name: shm, mountPath: /dev/shm }
      resources:
        requests: { memory: 2Gi }
        limits: { memory: 2Gi }   # php-fpm workers + nocopy.shm_size
  volumes:
    - name: shm
      emptyDir:
        medium: Memory
        sizeLimit: 160Mi          # a bit above nocopy.shm_size
```

The cache's memory counts against the container's memory limit, all of it from the start. Budget the limit as
`pm.max_children × memory per worker + nocopy.shm_size`. If the volume is too small, PHP warns `not enough room in
/dev/shm` at startup and runs without a cache.

The volume lives as long as the pod, so the cache survives a php-fpm or container restart. A new image builds a new
`nocopy.so`, and so a new, empty cache ([Upgrade](install.md#upgrade)).

## 2. Run every process as the pool's user

Only the user that created the cache can open it ([why](share-between-fpm-and-cli.md#2-run-everything-as-one-user)).
`kubectl exec`, exec probes and CronJobs run as the container's user, which is root unless you set one. So run the
container as the pool's user:

```yaml
securityContext:
  runAsUser: 33     # www-data on Debian-based images; 82 on Alpine
  runAsGroup: 33
```

php-fpm then runs its master as that user too (it logs that the pool's `user` directive is ignored). If the container
must start as root, run each command as the pool's user instead:

```sh
kubectl exec deploy/php -- su -s /bin/sh www-data -c 'vendor/bin/drush cr'
```

## 3. Configure from a ConfigMap and a Secret

PHP expands `${VAR}` in ini files, so the RESP password stays in a Secret:

```yaml
apiVersion: v1
kind: ConfigMap
metadata: { name: nocopy-ini }
data:
  nocopy.ini: |
    nocopy.shm_name=/nocopy
    nocopy.shm_size=128M
    nocopy.resp=${NOCOPY_RESP}
    nocopy.resp_prefix=${NOCOPY_RESP_PREFIX}
```

```yaml
      env:
        - name: NOCOPY_RESP
          valueFrom: { secretKeyRef: { name: resp, key: url } }   # tcp://user:secret@resp:6379
        - name: NOCOPY_RESP_PREFIX
          value: "mysite-production:"
      volumeMounts:
        - { name: nocopy-ini, mountPath: /usr/local/etc/php/conf.d/zz-nocopy.ini, subPath: nocopy.ini }
```

On the official PHP images, php-fpm and the CLI read the same `conf.d`. Give CronJobs the same file and variables: a
CronJob without `nocopy.resp` has a cache of its own, and its writes and invalidations never reach the web pods.

- Environments that share one RESP (staging, production) need different `nocopy.resp_prefix` values.
- All pods of one environment need the same prefix and the same `nocopy.serializer`.
- An unset variable expands to an empty value, and an empty `nocopy.resp` means no RESP.

## 4. Size RESP

- Connections: one per php-fpm worker or CLI process, plus one listener per pod. Keep RESP's `maxclients` above
  `replicas × (pm.max_children + 1)` summed over every deployment, plus CronJobs.
- Memory: set `maxmemory` below the RESP container's limit, and `maxmemory-policy volatile-lfu` for the Drupal module
  ([why](../../drupal/README.md)).

## 5. Check it

```sh
kubectl exec deploy/php -- php -r 'var_dump(nocopy_shared_with());'   # "resp" (one pod alone: "host")
kubectl exec deploy/php -- ps ax | grep nocopy-resp                  # the pod's listener
kubectl exec deploy/php -- df -h /dev/shm                              # the volume, not 64M
```

Then write in one pod and read in another, as in [RESP](resp.md#2-restart-and-verify). Watch `nocopy_stats()` in
each pod ([Size and monitor](size-and-monitor.md)): each pod has its own local copies, and so its own `used` and `evicted`.
