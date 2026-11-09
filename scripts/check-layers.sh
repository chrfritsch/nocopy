#!/bin/sh
# The arena's boundaries (docs/explanation/architecture.md, Layers): headers include only lower layers; only arena.c and layout.c
# know the layout; outside arena/ only arena/nocopy_arena.h is included and no ar_/AR_ name is used.
cd "$(dirname "$0")/.." || exit 2
rank() {
	case $1 in
	base.h) echo 0 ;;
	clock.h | proc.h | region.h) echo 1 ;;
	entry.h | index_read.h | pool_read.h | leases.h) echo 2 ;;
	index.h | pool.h | atoms_read.h) echo 3 ;;
	log.h | atoms.h | ctx.h) echo 4 ;;
	policy_read.h) echo 5 ;;
	policy.h) echo 6 ;;
	shard.h) echo 7 ;;
	layout.h | nocopy_arena.h) echo 8 ;;
	*) echo unknown ;;
	esac
}
bad=0
no() { echo "check-layers: $*"; bad=1; }
for f in arena/*.h; do
	r=$(rank "${f#arena/}")
	[ "$r" = unknown ] && no "$f has no layer: add it to scripts/check-layers.sh"
	for i in $(sed -n 's/^#include "\(.*\)"/\1/p' "$f"); do
		[ "$(rank "$i")" -lt "$r" ] 2>/dev/null || no "$f includes $i: not a lower layer"
	done
done
for f in arena/*.c; do
	case $f in arena/arena.c | arena/layout.c) continue ;; esac
	grep -qE '#include "(layout|nocopy_arena)\.h"' "$f" && no "$f includes layout.h or nocopy_arena.h: modules do not know the layout"
done
grep -nE '#include "arena/' -- *.c *.h | grep -v '"arena/nocopy_arena.h"' | while read -r l; do echo "check-layers: $l: include arena/nocopy_arena.h only"; done | grep . && bad=1
grep -nE '\b(ar|AR)_[A-Za-z]' -- *.c *.h | sed 's/^/check-layers: internal name: /' | grep . && bad=1
[ $bad = 0 ] && echo "check-layers: ok"
exit $bad
