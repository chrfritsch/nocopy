<?php
// After nocopy_clear() with no other process attached, everything must be back in the pool: what is left over is listed (empty: none).
function arena_leftovers(): array
{
    nocopy_clear();
    $s = nocopy_stats();
    $left = [];
    foreach (['entries', 'used', 'dead_segments', 'pinned_segments', 'kept_segments', 'leases'] as $k) {
        if ($s[$k] !== 0) $left[] = "$k=$s[$k]";
    }
    if ($s['free_segments'] !== $s['segments']) $left[] = "free_segments=$s[free_segments] of $s[segments]";
    return $left;
}
