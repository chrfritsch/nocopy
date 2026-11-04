<?php
// One request: store/fetch, report whether the fetch was zero-copy and how many segments stay pinned.
function serve(): void {
    $n = (int) ($_GET['n'] ?? 0);
    nocopy_put("k$n", ['n' => $n, 'list' => range(1, 300)]);
    nocopy_put('churn', range(1, 2000)); // makes garbage: pins that are never released would keep it
    $v = nocopy_get("k$n");
    ob_start(); debug_zval_dump($v); $zero = str_contains(ob_get_clean(), 'interned');
    echo json_encode(['ok' => $v['n'] === $n, 'zero_copy' => $zero, 'pinned' => nocopy_stats()['pinned_segments'], 'zts' => PHP_ZTS]), "\n";
}
if (function_exists('frankenphp_handle_request') && ($_SERVER['FRANKENPHP_WORKER'] ?? false)) {
    for ($i = 0; frankenphp_handle_request('serve') && $i < 100000; $i++);
} else {
    serve();
}
