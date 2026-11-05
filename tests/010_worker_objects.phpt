--TEST--
apcu_*: worker mode copies on fetch and tolerates re-entrant unserialize
--EXTENSIONS--
nocopy
--INI--
nocopy.shm_name=/nocopy-t010
nocopy.request_scope=0
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
class Reentrant {
    public $v = 'x';
    public function __wakeup() { apcu_fetch('side'); apcu_store('side2', 1); }
}
apcu_store('side', ['a' => 1]);
apcu_store('r', new Reentrant);
for ($i = 0; $i < 200; $i++) apcu_fetch('r');
apcu_store('big', range(1, 1000));
ob_start(); debug_zval_dump(apcu_fetch('big')); $d = ob_get_clean();
var_dump(str_contains($d, 'interned'));
for ($i = 0; $i < 100; $i++) apcu_store('churn', range(1, 1000));
$s = nocopy_stats();
var_dump($s['pinned_segments'], $s['used'] < 200000);
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t010*')); ?>
--EXPECT--
bool(false)
int(0)
bool(true)
