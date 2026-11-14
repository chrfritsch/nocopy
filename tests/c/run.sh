#!/bin/sh
# Builds tests/c/stress.c against the arena units twice (-O2, and ThreadSanitizer) and runs both on small and sharded arenas; then tests/c/repair.c, tests/c/atoms.c, tests/c/merge.c,
# tests/c/crash.c and tests/c/sched.c (both with the arena's test hooks, -DNOCOPY_HOOKS); stress, merge and crash again with keys of 32 hashes (NOCOPY_COLLIDE).
# Usage: tests/c/run.sh [seconds=3]   (Linux; e.g. docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/tests/c/run.sh)
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/nocopy-c; mkdir -p "$out"
CC=${CC:-cc}
ARENA=$(ls arena/*.c)
# build <binary> <test> <flags...>: tests/c/<test>.c with the arena units
build() { bin=$1 src=$2; shift 2; $CC -g -DNOCOPY_POISON "$@" -o "$out/$bin" "tests/c/$src.c" $ARENA -lpthread; }
# every arena function is static or declared in its module's header, and no name hides another (PHP's own macros rule these out for the extension build)
W="-Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes -Wshadow -Werror"
build stress stress -O2 $W
build stress-tsan stress -O1 -fsanitize=thread -Wno-tsan
s=${1:-3}
for mb in 4 64; do "$out/stress" $mb 8 "$s"; done
for mb in 4 64; do TSAN_OPTIONS=halt_on_error=1 "$out/stress-tsan" $mb 8 "$s"; done
for t in repair atoms merge; do build $t $t -O1 $W; "$out/$t"; done
for t in crash sched; do build $t $t -O1 $W -DNOCOPY_HOOKS; "$out/$t"; done
# colliding keys: alternative hashes under threads, merges and kills
build stress-collide stress -O2 $W -DNOCOPY_COLLIDE=63
build stress-collide-tsan stress -O1 -fsanitize=thread -Wno-tsan -DNOCOPY_COLLIDE=63
for mb in 4 64; do "$out/stress-collide" $mb 8 "$s"; done
TSAN_OPTIONS=halt_on_error=1 "$out/stress-collide-tsan" 64 8 "$s"
build merge-collide merge -O1 $W -DNOCOPY_COLLIDE=63; "$out/merge-collide"
build crash-collide crash -O1 $W -DNOCOPY_HOOKS -DNOCOPY_COLLIDE=63; "$out/crash-collide" all
