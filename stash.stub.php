<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80300
 */

/** $serialize: objects and shared references are stored serialized instead of throwing */
function stash_put(string $key, mixed $value, int $ttl = 0, bool $serialize = false): bool {}

function stash_get(string $key, mixed &$hit = null): mixed {}

function stash_delete(string $key): bool {}

function stash_checkpoint(): void {}

/** Fetch, or compute once across workers and store. $beta > 0 refreshes early (XFetch), scaled by the measured compute time. */
function stash_remember(string $key, callable $callback, int $ttl = 0, float $beta = 1.0): mixed {}

function stash_scope(callable $callback): mixed {}

function stash_stats(): array {}
