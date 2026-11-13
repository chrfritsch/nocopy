--TEST--
nocopy.resp: the payload in RESP is a wire format (0x00, flags, int64 expires, uint32 compute ms, the serialized value; integers decimal): written and read byte for byte
--EXTENSIONS--
nocopy
--SKIPIF--
<?php require __DIR__ . '/resp_skip.inc'; ?>
--INI--
nocopy.shm_name=/nocopy-t119
nocopy.resp=127.0.0.1:6379
nocopy.resp_prefix=t119:
nocopy.serializer=php
--CONFLICTS--
resp
--FILE--
<?php
require __DIR__ . '/resp.inc';
nocopy_clear();
// written: the header, then serialize()'s bytes
nocopy_put('w', ['a' => 1], 100);
$raw = vk('GET', 't119:w');
$h = unpack('Cz/Cflags/Pexpires/Vms', $raw);
var_dump($h['z'], $h['flags'], abs($h['expires'] - (time() + 100)) <= 1, $h['ms'], substr($raw, 14) === serialize(['a' => 1]));
nocopy_put('n', 42);
var_dump(vk('GET', 't119:n'));
// read: a payload built by hand (as an older build, or another server, wrote it)
vk('SET', 't119:r', "\0\0" . pack('P', 0) . pack('V', 1234) . serialize(['b' => [2, 3]]));
vk('SET', 't119:i', '-7');
settle();
var_dump(nocopy_get('r'), nocopy_get('i'));
vk('SET', 't119:bad', "\0\0" . 'short');
settle();
var_dump(nocopy_get('bad', $hit), $hit);
vk('DEL', 't119:w', 't119:n', 't119:r', 't119:i', 't119:bad');
?>
--CLEAN--
<?php array_map('unlink', glob('/dev/shm/nocopy-t119*')); ?>
--EXPECT--
int(0)
int(0)
bool(true)
int(0)
bool(true)
string(2) "42"
array(1) {
  ["b"]=>
  array(2) {
    [0]=>
    int(2)
    [1]=>
    int(3)
  }
}
int(-7)
NULL
bool(false)
