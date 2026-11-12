<?php
// RESP tier encodings: serialize vs igbinary, ns and bytes; writes each payload to $argv[1] for bench/codecs.c. Needs ext-igbinary.

require __DIR__ . '/values.inc';
$codecs = [
    'serialize' => ['serialize', 'unserialize'],
    'igbinary' => ['igbinary_serialize', 'igbinary_unserialize'],
];

foreach ($values as $name => $v) {
    foreach ($codecs as $c => [$enc, $dec]) {
        $s = $enc($v);
        if ($dec($s) != $v) exit("round trip failed: $name $c\n");
        file_put_contents("$argv[1]/$name.$c", $s);
        printf("%s\t%s\t%d\t%.0f\t%.0f\n", $name, $c, strlen($s), ns($enc, $v), ns($dec, $s));
    }
}
