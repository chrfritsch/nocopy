#!/bin/sh
# Builds tests/c/stress.c against the arena units twice (-O2, and ThreadSanitizer) and runs both on small and sharded arenas; then tests/c/repair.c and tests/c/atoms.c.
# Usage: tests/c/run.sh [seconds=3]   (Linux; e.g. docker run --rm -v "$PWD":/src:ro nocopy-linux:8.5 /src/tests/c/run.sh)
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/nocopy-c; mkdir -p "$out"
CC=${CC:-cc}
ARENA="nocopy_arena.c nocopy_evict.c nocopy_lease.c nocopy_owner.c nocopy_atom.c"
$CC -O2 -g -Wall -Wextra -DNOCOPY_POISON -o "$out/stress" tests/c/stress.c $ARENA -lpthread
$CC -O1 -g -fsanitize=thread -Wno-tsan -DNOCOPY_POISON -o "$out/stress-tsan" tests/c/stress.c $ARENA -lpthread
s=${1:-3}
for mb in 4 64; do "$out/stress" $mb 8 "$s"; done
for mb in 4 64; do TSAN_OPTIONS=halt_on_error=1 "$out/stress-tsan" $mb 8 "$s"; done
$CC -O1 -g -Wall -Wextra -DNOCOPY_POISON -o "$out/repair" tests/c/repair.c $ARENA -lpthread && "$out/repair"
$CC -O1 -g -Wall -Wextra -DNOCOPY_POISON -o "$out/atoms" tests/c/atoms.c $ARENA -lpthread && "$out/atoms"
