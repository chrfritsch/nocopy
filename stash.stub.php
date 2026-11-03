<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80300
 */

function stash_put(string $key, mixed $value, int $ttl = 0): bool {}

function stash_get(string $key, mixed &$hit = null): mixed {}

function stash_delete(string $key): bool {}

function stash_checkpoint(): void {}

function stash_scope(callable $callback): mixed {}

function stash_stats(): array {}
