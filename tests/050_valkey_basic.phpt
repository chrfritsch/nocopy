--TEST--
nocopy.valkey: values, integers, add, TTLs, delete and clear go to Valkey; a change by another client drops the local copy
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/valkey_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t050
nocopy.valkey=127.0.0.1:6379
nocopy.valkey_prefix=t050:
--FILE--
<?php
require __DIR__ . '/../apcu/apcu.php';
require __DIR__ . '/valkey.inc';
apcu_clear_cache();
var_dump(nocopy_put('a', ['x' => [1, 2]]), nocopy_get('a'), ord(vk('GET', 't050:a')[0]));
var_dump(nocopy_put('o', new ArrayObject([1]), 0, true), nocopy_get('o') instanceof ArrayObject);
var_dump(apcu_store('n', 5), apcu_inc('n'), apcu_dec('n', 2), vk('GET', 't050:n'), apcu_cas('n', 4, 10), apcu_fetch('n'));
var_dump(apcu_inc('fresh', 3, $ok, 100), $ok, vk('PTTL', 't050:fresh') > 90000);
var_dump(apcu_add('a', 1), apcu_add('new', 1), apcu_fetch('new'));
var_dump(nocopy_put('t', 'v', 50), vk('PTTL', 't050:t') > 40000, apcu_exists('t'));
var_dump(nocopy_delete('a'), nocopy_get('a', $hit), $hit, vk('EXISTS', 't050:a'));
apcu_clear_cache();
var_dump(vk('EXISTS', 't050:n'), apcu_fetch('t'));

nocopy_put('k', 'old');
nocopy_get('k');
vk('SET', 't050:k', '7');
settle();
var_dump(nocopy_get('k'));
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t050*')); ?>
--EXPECT--
bool(true)
array(1) {
  ["x"]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(2)
  }
}
int(0)
bool(true)
bool(true)
bool(true)
int(6)
int(4)
string(1) "4"
bool(true)
int(10)
int(3)
bool(true)
bool(true)
bool(false)
bool(true)
int(1)
bool(true)
bool(true)
bool(true)
bool(true)
NULL
bool(false)
int(0)
int(0)
bool(false)
int(7)
