<?php

// apcu_* on top of nocopy_*, for code that cannot switch yet. Objects are stored serialized, as in APCu.
// apc.enabled/apc.enable_cli come from the extension and only feed apcu_enabled(), as callers check them.
// ponytail: no apcu_key_info or other apc.* INI; cache_info/sma_info are summaries without entry lists.

// conditional: top-level functions are bound at compile time, so a guard with return cannot stop a second include
// (php -S runs auto_prepend_file for the router and again for the script it routes to)
if (function_exists('nocopy_get') && !extension_loaded('apcu') && !function_exists('apcu_fetch')) {

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

function apcu_delete(mixed $key): array|bool
{
    if ($key instanceof APCUIterator) {
        foreach (array_keys($key->keys()) as $k) {
            nocopy_delete((string) $k);
        }
        return true;
    }
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

define('APC_LIST_ACTIVE', 1);
define('APC_LIST_DELETED', 2);
define('APC_ITER_TYPE', 1);
define('APC_ITER_KEY', 2);
define('APC_ITER_VALUE', 4);
define('APC_ITER_NUM_HITS', 8);
define('APC_ITER_MTIME', 16);
define('APC_ITER_CTIME', 32);
define('APC_ITER_DTIME', 64);
define('APC_ITER_ATIME', 128);
define('APC_ITER_REFCOUNT', 256);
define('APC_ITER_MEM_SIZE', 512);
define('APC_ITER_TTL', 1024);
define('APC_ITER_NONE', 0);
define('APC_ITER_ALL', 0xffffffff);

/** Entries of nocopy_keys(), taken at the first use or rewind; values fetched as current() reaches them. No hit counts (0). */
if (!class_exists('APCUIterator', false)) {
class APCUIterator implements Iterator
{
    private ?array $entries = null;
    private array $names = [];
    private int $pos = 0;
    private bool $ready = false;
    private string|array|null $search;
    private int $format;
    private int $list;

    public function __construct(array|string|null $search = null, int $format = APC_ITER_ALL, int $chunk_size = 0, int $list = APC_LIST_ACTIVE)
    {
        if (!apcu_enabled()) {
            throw new Error('APC must be enabled to use APCUIterator');
        }
        if ($chunk_size < 0) {
            trigger_error('APCUIterator chunk size must be 0 or greater', E_USER_WARNING);
            return;
        }
        if ($list !== APC_LIST_ACTIVE && $list !== APC_LIST_DELETED) {
            trigger_error('APCUIterator invalid list type', E_USER_WARNING);
            return;
        }
        $this->search = is_array($search) ? array_flip(array_map('strval', $search)) : ($search === '' ? null : $search);
        $this->format = $format;
        $this->list = $list;
        $this->ready = true;
    }

    /** @internal the matching entries: key => nocopy_keys() info */
    public function keys(): array
    {
        if (!$this->ready) {
            throw new Error('Trying to use uninitialized APCUIterator');
        }
        if ($this->entries === null) {
            $all = $this->list === APC_LIST_ACTIVE ? nocopy_keys() : [];
            $this->entries = match (true) {
                $this->search === null => $all,
                is_array($this->search) => array_intersect_key($all, $this->search),
                default => array_filter($all, fn ($k) => preg_match($this->search, (string) $k), ARRAY_FILTER_USE_KEY),
            };
            $this->names = array_map('strval', array_keys($this->entries));
        }
        return $this->entries;
    }

    public function rewind(): void
    {
        $this->entries = null;
        $this->keys();
        $this->pos = 0;
    }

    public function next(): void
    {
        $this->keys();
        $this->pos++;
    }

    public function valid(): bool
    {
        return $this->pos < count($this->keys());
    }

    public function key(): string|int
    {
        if (!$this->valid()) {
            throw new Error('Cannot call key() on invalid iterator');
        }
        return $this->names[$this->pos];
    }

    public function current(): mixed
    {
        if (!$this->valid()) {
            throw new Error('Cannot call current() on invalid iterator');
        }
        $k = $this->names[$this->pos];
        $e = $this->entries[$k];
        $f = $this->format;
        $item = [];
        $f & APC_ITER_TYPE && $item['type'] = 'user';
        $f & APC_ITER_KEY && $item['key'] = $k;
        $f & APC_ITER_VALUE && $item['value'] = nocopy_get($k);
        $f & APC_ITER_NUM_HITS && $item['num_hits'] = 0;
        $f & APC_ITER_MTIME && $item['mtime'] = $e['created'];
        $f & APC_ITER_CTIME && $item['creation_time'] = $e['created'];
        $f & APC_ITER_DTIME && $item['deletion_time'] = 0;
        $f & APC_ITER_ATIME && $item['access_time'] = $e['created'];
        $f & APC_ITER_REFCOUNT && $item['ref_count'] = 0;
        $f & APC_ITER_MEM_SIZE && $item['mem_size'] = $e['size'];
        $f & APC_ITER_TTL && $item['ttl'] = $e['expires'] ? $e['expires'] - $e['created'] : 0;
        return $item;
    }

    public function getTotalHits(): int
    {
        $this->keys();
        return 0;
    }

    public function getTotalSize(): int
    {
        return array_sum(array_column($this->keys(), 'size'));
    }

    public function getTotalCount(): int
    {
        return count($this->keys());
    }
}
}
}
