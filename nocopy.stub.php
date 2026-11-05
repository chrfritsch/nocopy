<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80300
 */

/**
 * $serialize: objects and shared references are stored serialized instead of throwing
 * @frameless-function {"arity": 2}
 * @frameless-function {"arity": 3}
 */
function nocopy_put(string $key, mixed $value, int $ttl = 0, bool $serialize = false): bool {}

/**
 * @frameless-function {"arity": 1}
 */
function nocopy_get(string $key, mixed &$hit = null): mixed {}

function nocopy_delete(string $key): bool {}

/** The hits as key => value; with nocopy.valkey, every local miss in one round trip. */
function nocopy_get_multiple(array $keys): array {}

/** key => value, one $ttl for all; with nocopy.valkey, one pipeline. false: some were not stored. */
function nocopy_put_multiple(array $values, int $ttl = 0, bool $serialize = false): bool {}

/** Fetch, or compute once across workers and store. $beta > 0 refreshes early (XFetch), scaled by the measured compute time. */
function nocopy_remember(string $key, callable $callback, int $ttl = 0, float $beta = 1.0): mixed {}

/**
 * Who sees this process's writes: "host" (every process on this machine that attached to nocopy.shm_name), "valkey" (every process on the
 * same nocopy.valkey and prefix), "none" (nocopy.shm_name not set or not attached: nothing is kept or served).
 */
function nocopy_shared_with(): string {}

function nocopy_stats(): array {}
