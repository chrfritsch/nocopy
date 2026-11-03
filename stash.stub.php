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

function stash_scope(callable $callback): mixed {}

function stash_stats(): array {}
