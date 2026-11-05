#!/bin/sh
# Fuzzes the RESP3 parser (nocopy_resp.h) with libFuzzer + ASan + UBSan; stops at the first crash.
# Usage: tests/c/fuzz.sh [seconds=60]   (needs clang; ./dev fuzz)
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/nocopy-fuzz; mkdir -p "$out/corpus"
clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -o "$out/fuzz_resp" tests/c/fuzz_resp.c
i=0
for s in '+OK\r\n' '-ERR x\r\n' ':42\r\n' '$3\r\nabc\r\n' '$-1\r\n' '_\r\n' '*2\r\n$1\r\na\r\n:1\r\n' \
         '%1\r\n+k\r\n+v\r\n' '>2\r\n$10\r\ninvalidate\r\n*1\r\n$3\r\nkey\r\n' '~1\r\n,1.5\r\n' '=7\r\ntxt:abc\r\n' \
         '(\n' '$\r\n'; do  # the last two: past bugs (len underflow, strtoll past the buffer)
	printf %b "$s" > "$out/corpus/seed$i"; i=$((i + 1))
done
cd "$out" && ./fuzz_resp -max_total_time="${1:-60}" -max_len=4096 -print_final_stats=1 corpus
