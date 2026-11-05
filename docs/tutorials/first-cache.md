# Your first cache

In this tutorial you build nocopy, store a value from the command line, read it back from a web server, and watch a
10,000-row array come out of the cache in nanoseconds where APCu needs a millisecond.

You need Docker and a checkout of this repository. You do not need PHP on your machine. Takes about 10 minutes.

## 1. Start a shell with nocopy loaded

From the repository root:

```sh
./dev shell
```

The first run builds a Linux image with PHP 8.5 (a few minutes); later runs start in seconds. You land in a bash prompt
inside the container, with nocopy compiled and loaded and the repository at `/src`. The shell's `php.ini` already contains:

```ini
extension=/build/modules/nocopy.so
nocopy.shm_name=/nocopy-shell
```

`nocopy.shm_name` names the shared memory that holds the cache. Every PHP process with the same name uses the same cache.

## 2. Store and read a value

```sh
php -r 'var_dump(nocopy_shared_with());'
```

```
string(4) "host"
```

`host`: this process shares its cache with every other PHP process on this machine that uses the same name. Now store a value:

```sh
php -r 'nocopy_put("greeting", "hello"); var_dump(nocopy_get("greeting"));'
```

```
string(5) "hello"
```

Run a **new** process that only reads:

```sh
php -r 'var_dump(nocopy_get("greeting"));'
```

```
string(5) "hello"
```

The first process has exited, and the value is still there. It lives in shared memory, not in the process.

## 3. Cache something big

Create a small page that builds 10,000 rows on a miss and then fetches them 1,000 times:

```sh
mkdir -p /tmp/demo && cat > /tmp/demo/index.php <<'EOF'
<?php
$rows = nocopy_get("products", $hit);
if (!$hit) {
    $rows = [];
    for ($i = 0; $i < 10000; $i++) {
        $rows[] = ["id" => $i, "name" => "Product $i", "price" => $i * 1.5];
    }
    nocopy_put("products", $rows, ttl: 300);
}
$t = hrtime(true);
for ($i = 0; $i < 1000; $i++) {
    $rows = nocopy_get("products");
}
printf("%s, %d rows, %.0f ns per fetch\n", $hit ? "hit" : "miss", count($rows), (hrtime(true) - $t) / 1000);
EOF
```

`$hit` tells a miss apart from a stored `null` or `false`.

## 4. Serve it from a web server

Start PHP's built-in web server in the background and request the page twice:

```sh
php -S 127.0.0.1:8080 -t /tmp/demo >/dev/null 2>&1 &
curl -s 127.0.0.1:8080; curl -s 127.0.0.1:8080
```

```
miss, 10000 rows, 396 ns per fetch
hit, 10000 rows, 53 ns per fetch
```

Your numbers will differ; the order of magnitude will not. The 10,000 rows are not copied: `nocopy_get()` returns a pointer
into shared memory. That is **zero-copy mode**, used by default under web servers (php-fpm, Apache, FrankenPHP classic, `php -S`).

## 5. Read the same cache from the CLI

The web server stored `products`. Read it from the command line:

```sh
php /tmp/demo/index.php
```

```
hit, 10000 rows, 862 ns per fetch
```

A hit: the CLI and the web server share one cache. The CLI is slower because it runs in **copy mode**: a long-running
script could keep a value forever, so it gets its own copy, and nocopy reuses that copy while the entry is unchanged.
You can force zero-copy for a short script:

```sh
php -d nocopy.request_scope=1 /tmp/demo/index.php
```

```
hit, 10000 rows, 17 ns per fetch
```

## 6. Compare with APCu

The image has APCu too. Same rows, same loop:

```sh
php -d extension=$(find /usr/local/lib/php/extensions -name apcu.so) -d apc.enable_cli=1 -r '
$r = []; for ($i = 0; $i < 10000; $i++) $r[] = ["id" => $i, "name" => "Product $i", "price" => $i * 1.5];
apcu_store("p", $r);
$t = hrtime(true); for ($i = 0; $i < 1000; $i++) $x = apcu_fetch("p");
printf("APCu: %.0f ns per fetch\n", (hrtime(true) - $t) / 1000);'
```

```
APCu: 1337556 ns per fetch
```

About 1.3 ms: APCu rebuilds the whole array on every fetch.

## 7. Look inside

```sh
php -r 'print_r(array_intersect_key(nocopy_stats(), array_flip(["entries", "used", "hits", "misses", "shm_name"])));'
```

```
Array
(
    [used] => 2720400
    [entries] => 2
    [hits] => 4005
    [misses] => 1
    [shm_name] => /nocopy-shell.0927d4a6
)
```

Two entries (`greeting` and `products`), about 2.7 MB used, and the hits of every process so far. Type `exit` to leave;
the container and its cache are gone.

## What you learned

- A cache is a named piece of shared memory (`nocopy.shm_name`), shared by every PHP process that uses the name.
- Web requests get values without a copy; the CLI and long-running workers get a reused private copy.
- `nocopy_put()`, `nocopy_get()` with `$hit`, and `nocopy_stats()`.

Next: [install nocopy on a server](../how-to/install.md), read [how nocopy works](../explanation/how-it-works.md),
or browse all [functions](../reference/functions.md).
