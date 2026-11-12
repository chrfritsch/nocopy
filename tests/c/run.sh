#!/bin/sh
# Builds tests/c/stress.c against the arena units twice (-O2, and ThreadSanitizer) and runs both on small and sharded arenas; then tests/c/repair.c, tests/c/atoms.c, tests/c/merge.c,
# tests/c/crash.c and tests/c/sched.c (both with the arena's test hooks, -DNOCOPY_HOOKS).
# Usage: tests/c/run.sh [seconds=3]   (Linux; e.g. docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/tests/c/run.sh)
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/nocopy-c; mkdir -p "$out"
CC=${CC:-cc}
ARENA=$(ls arena/*.c)
# build <binary> <test> <flags...>: tests/c/<test>.c with the arena units
build() { bin=$1 src=$2; shift 2; $CC -g -DNOCOPY_POISON "$@" -o "$out/$bin" "tests/c/$src.c" $ARENA -lpthread; }
build stress stress -O2 -Wall -Wextra
build stress-tsan stress -O1 -fsanitize=thread -Wno-tsan
s=${1:-3}
for mb in 4 64; do "$out/stress" $mb 8 "$s"; done
for mb in 4 64; do TSAN_OPTIONS=halt_on_error=1 "$out/stress-tsan" $mb 8 "$s"; done
for t in repair atoms merge; do build $t $t -O1 -Wall -Wextra && "$out/$t"; done
for t in crash sched; do build $t $t -O1 -Wall -Wextra -DNOCOPY_HOOKS && "$out/$t"; done
