<?php

// apcu_* on top of nocopy_*, for code that cannot switch yet. Objects are stored serialized, as in APCu.
// apc.enabled/apc.enable_cli come from the extension and only feed apcu_enabled(), as callers check them.
// ponytail: no APCUIterator, apcu_key_info or other apc.* INI; cache_info/sma_info are summaries without entry lists.

if (!function_exists('nocopy_get') || function_exists('apcu_fetch')) {
    return;
}

function apcu_enabled(): bool
{
    return nocopy_shared_with() !== 'none' && ini_get('apc.enabled') && (PHP_SAPI !== 'cli' || ini_get('apc.enable_cli'));
}

function apcu_store(array|string|int $key, mixed $value = null, int $ttl = 0): array|bool
{
    return apcu_put('apcu_store', 'nocopy_put', $key, $value, $ttl, func_num_args());
}

function apcu_add(array|string|int $key, mixed $value = null, int $ttl = 0): array|bool
{
    return apcu_put('apcu_add', 'nocopy_add', $key, $value, $ttl, func_num_args());
}

/** @internal apcu_store/apcu_add with $put as the store. */
function apcu_put(string $fn, callable $put, array|string|int $key, mixed $value, int $ttl, int $argc): array|bool
{
    if ($ttl < 0) {
        throw new ValueError("$fn(): Argument #3 (\$ttl) must be greater than or equal to 0");
    }
    if (!is_array($key)) {
        if ($argc < 2) {
            throw new ArgumentCountError("$fn() expects at least 2 arguments, 1 given");
        }
        return $put((string) $key, $value, $ttl);
    }
    $failed = [];
    foreach ($key as $k => $v) {
        if (!$put((string) $k, $v, $ttl)) {
            $failed[$k] = -1;
        }
    }
    return $failed;
}

function apcu_fetch(array|string|int $key, mixed &$success = null): mixed
{
    if (is_array($key)) {
        $success = true;
        return nocopy_get_multiple(array_map('strval', $key));
    }
    $v = nocopy_get((string) $key, $hit);
    $success = $hit;
    return $hit ? $v : false;
}

function apcu_exists(array|string|int $key): array|bool
{
    if (is_array($key)) {
        return array_fill_keys(array_keys(nocopy_get_multiple(array_map('strval', $key))), true);
    }
    nocopy_get((string) $key, $hit);
    return $hit;
}

function apcu_delete(array|string|int $key): array|bool
{
    if (!is_array($key)) {
        return nocopy_delete((string) $key);
    }
    $failed = [];
    foreach ($key as $k) {
        if (!nocopy_delete((string) $k)) {
            $failed[] = $k;
        }
    }
    return $failed;
}

function apcu_inc(string $key, int $step = 1, mixed &$success = null, int $ttl = 0): int|false
{
    $r = nocopy_inc($key, $step, $ttl);
    $success = $r !== false;
    return $r;
}

function apcu_dec(string $key, int $step = 1, mixed &$success = null, int $ttl = 0): int|false
{
    return apcu_inc($key, -$step, $success, $ttl);
}

function apcu_cas(string $key, int $old, int $new): bool
{
    return nocopy_cas($key, $old, $new);
}

function apcu_clear_cache(): bool
{
    return nocopy_clear();
}

function apcu_entry(string $key, callable $callback, int $ttl = 0): mixed
{
    return nocopy_remember($key, $callback, $ttl, 0.0);
}

function apcu_cache_info(bool $limited = false): array|false
{
    $s = nocopy_stats();
    if (!$s['size']) {
        return false;
    }
    $info = [
        'num_slots' => $s['slots'],
        'ttl' => 0,
        'num_hits' => $s['hits'],
        'num_misses' => $s['misses'],
        'num_entries' => $s['entries'],
        'expunges' => $s['expunges'],
        'mem_size' => $s['used'],
        'memory_type' => 'mmap',
    ];
    return $limited ? $info : $info + ['cache_list' => [], 'deleted_list' => [], 'slot_distribution' => []];
}

function apcu_sma_info(bool $limited = false): array|false
{
    $s = nocopy_stats();
    if (!$s['size']) {
        return false;
    }
    $info = ['num_seg' => 1, 'seg_size' => $s['size'], 'avail_mem' => $s['size'] - $s['used']];
    return $limited ? $info : $info + ['block_lists' => [[]]];
}
