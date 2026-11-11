#!/bin/sh
# Inside nocopy-perf: table from /out/cg-*.diff: total Ir per call and the three biggest functions (N as for cg.sh).
n=${N:-20000}
for op in ${OPS:-$(cd /out && ls cg-*.diff | sed 's/^cg-//; s/\.diff$//')}; do
  d=/out/cg-$op.diff
  per=$(php -n -d extension=/build/modules/nocopy.so -d nocopy.shm_name=/nocopy-cg /src/bench/prof.php $op per 2>/dev/null | tail -1)
  cg_annotate --show-percs=no $d 2>/dev/null | awk -v n=$n -v per=${per:-1} -v op=$op '
    function f(x) { gsub(",", "", x); return x / n * per }
    /PROGRAM TOTALS/ { t = f($1) }
    /^-- Function:file summary/ { seen = 1; next }
    /^-- Annotated/ { exit }
    seen && /^> +[0-9,]+ +[A-Za-z_]/ { if (k < 4) { x = $3; sub(/:.*/, "", x); sub(/\.(lto_priv|constprop|isra|part)\.[0-9]+/, "", x); fn = fn sprintf(" %s %.0f", x, f($2)); k++ } }
    END { printf "%-18s %8.0f Ir  |%s\n", op, t, fn }'
done
