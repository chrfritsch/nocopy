# Your first change

In this tutorial you make a small change to the extension's C code: `nocopy_stats()` gets a `start_time` key that says
when the cache was created. You write a test first, watch it fail, make it pass, and run the checks CI runs.

You need Docker and a checkout of this repository. No PHP, compiler or Valkey on your machine: `./dev` runs everything in
a Linux container. Takes about 15 minutes, most of it waiting for builds.

## 1. Run the test suite once

```sh
./dev test
```

The first run builds the image (a few minutes). Every run copies the sources into the container, compiles the extension
from clean and runs the `.phpt` tests in `tests/`. At the end you see a summary like this (the counts change as the
suite grows, and tests that need Valkey may be skipped):

```
Tests skipped   :     2 (  1.9%) --------
Tests failed    :     0 (  0.0%) (  0.0%)
Tests passed    :   106 ( 98.1%) (100.0%)
```

What matters is `Tests failed: 0`. This is your baseline. Your checkout is mounted read-only, so the build never leaves files behind.

## 2. Write a failing test

Tests are PHP's `.phpt` files: a script and its expected output. Create `tests/900_start_time.phpt`:

```
--TEST--
nocopy_stats() reports when the cache was created
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t900
--FILE--
<?php
$s = nocopy_stats();
var_dump(is_int($s['start_time']), $s['start_time'] <= time());
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t900*')); ?>
--EXPECT--
bool(true)
bool(true)
```

Two conventions to keep in every test: each test uses its **own** `nocopy.shm_name` (tests run in parallel and must not
share a cache), and `--CLEAN--` removes that shared memory afterwards.

Run only this test:

```sh
./dev test tests/900_start_time.phpt
```

```
Tests failed    :     1 (100.0%) (100.0%)
== /build/tests/900_start_time.diff
001- bool(true)
001+ Warning: Undefined array key "start_time" in /build/tests/900_start_time.php on line 3
```

Lines with `-` are what the test expected, `+` what it got. The key does not exist yet.

## 3. Make it pass

The arena already records its creation time (`start_time` in `nocopy_stats_t`, `arena/nocopy_arena.h`); `nocopy_stats()` in
`nocopy.c` just does not return it. Find this line in `PHP_FUNCTION(nocopy_stats)`:

```c
	add_assoc_long(return_value, "leases", s.leases);
```

and add one below it:

```c
	add_assoc_long(return_value, "start_time", s.start_time);
```

Run the test again:

```sh
./dev test tests/900_start_time.phpt
```

```
Tests failed    :     0 (  0.0%) (  0.0%)
Tests passed    :     1 (100.0%) (100.0%)
```

## 4. Check you broke nothing

Run the whole suite, then the same suite built without the Valkey tier (the core must compile without it):

```sh
./dev test
./dev novalkey
```

Your change does not touch locking or memory reclamation. If it did, you would also run the race loops, which repeat the
concurrency tests because a race shows up only now and then:

```sh
./dev stress 30
./dev c 5
```

Before you open a pull request, run everything CI runs (PHP 8.5, thread-safe PHP, php-fpm, FrankenPHP, Symfony):

```sh
./dev ci
```

## 5. Try it by hand

```sh
./dev shell
php -r 'echo date("c", nocopy_stats()["start_time"]), "\n";'
```

`./dev shell` builds your current sources, so you see your change. Type `exit` to leave.

## 6. Clean up

This was an exercise. To drop it:

```sh
git checkout nocopy.c && rm tests/900_start_time.phpt
```

If you want to keep it as a real change, also add the key to the [`nocopy_stats()` reference](../reference/stats.md).

## What you learned

- `./dev test` builds from clean and runs the suite; give it a test path to run one test.
- A `.phpt` test: own `nocopy.shm_name`, `--CLEAN--`, expected output, and how to read its diff.
- Which checks a change needs: `./dev novalkey` always, `./dev stress` and `./dev c` for concurrency, `./dev ci` before a pull request.

Next: [Develop, test and benchmark](../how-to/develop.md) for the day-to-day tasks, and
[Architecture and invariants](../internals/architecture.md) before you change locking, eviction or the read path.
