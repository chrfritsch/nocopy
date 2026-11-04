<?php
// ?op=put&k=..: store from this server; ?op=get&k=..: what this server sees, and whether it came zero-copy.
function serve(): void {
    $k = $_GET['k'] ?? 'k';
    if (($_GET['op'] ?? 'get') === 'put') {
        nocopy_put($k, ['from' => PHP_SAPI, 'e' => [], 'rows' => range(1, 100)]);
    }
    $v = nocopy_get($k, $hit);
    ob_start(); debug_zval_dump($v["rows"] ?? null); $zero = str_contains(ob_get_clean(), 'interned');
    echo json_encode(['hit' => $hit, 'from' => $v['from'] ?? null, 'rows' => count($v['rows'] ?? []), 'zero_copy' => $zero, 'pid' => getmypid()]), "\n";
}
if (function_exists('frankenphp_handle_request') && ($_SERVER['FRANKENPHP_WORKER'] ?? false)) {
    for ($i = 0; frankenphp_handle_request('serve') && $i < 100000; $i++);
} else {
    serve();
}
