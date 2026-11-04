<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80300
 */

function apcu_enabled(): bool {}

/**
 * @param array|string $key
 * @frameless-function {"arity": 1}
 * @frameless-function {"arity": 2}
 * @frameless-function {"arity": 3}
 */
function apcu_store($key, mixed $value = UNKNOWN, int $ttl = 0): array|bool {}

/** @param array|string $key */
function apcu_add($key, mixed $value = UNKNOWN, int $ttl = 0): array|bool {}

/**
 * @param array|string $key
 * @param bool $success
 * @frameless-function {"arity": 1}
 */
function apcu_fetch($key, &$success = null): mixed {}

/**
 * @param array|string $key
 * @frameless-function {"arity": 1}
 */
function apcu_exists($key): array|bool {}

/**
 * @param APCUIterator|array|string $key
 * @frameless-function {"arity": 1}
 */
function apcu_delete($key): array|bool {}

/**
 * @param bool $success
 * @frameless-function {"arity": 1}
 * @frameless-function {"arity": 2}
 */
function apcu_inc(string $key, int $step = 1, &$success = null, int $ttl = 0): int|false {}

/**
 * @param bool $success
 * @frameless-function {"arity": 1}
 * @frameless-function {"arity": 2}
 */
function apcu_dec(string $key, int $step = 1, &$success = null, int $ttl = 0): int|false {}

function apcu_cas(string $key, int $old, int $new): bool {}

function apcu_clear_cache(): bool {}

function apcu_cache_info(bool $limited = false): array|false {}

function apcu_key_info(string $key): ?array {}

function apcu_sma_info(bool $limited = false): array|false {}

function apcu_entry(string $key, callable $callback, int $ttl = 0): mixed {}

class APCUIterator implements Iterator
{
    /** @param array|string|null $search */
    public function __construct($search = null, int $format = APC_ITER_ALL, int $chunk_size = 0, int $list = APC_LIST_ACTIVE) {}

    public function rewind(): void {}

    public function next(): void {}

    public function valid(): bool {}

    public function key(): string|int {}

    public function current(): mixed {}

    public function getTotalHits(): int {}

    public function getTotalSize(): int {}

    public function getTotalCount(): int {}
}
