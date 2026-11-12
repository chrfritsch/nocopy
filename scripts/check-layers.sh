#!/bin/sh
# The layers' boundaries (docs/internals/architecture.md, Layers). Arena: headers include only lower layers; only arena.c and layout.c
# know the layout; outside arena/ only arena/nocopy_arena.h is included and no ar_/AR_ name is used. Values: see below.
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
# The value layer (nocopy_value.h): no module state; nocopy_value_int.h is its own; writing runs under a shard lock.
value="nocopy_value.c nocopy_plan.c nocopy_write.c nocopy_read.c nocopy_class.c nocopy_serial.c nocopy_value.h nocopy_value_int.h"
for f in $value; do
	grep -nE '#include "nocopy_(php|kept)\.h"|\bNOCOPY_(G|SHARED)\b' "$f" | sed "s|^|check-layers: $f: module state in the value layer: |" | grep . && bad=1
done
for f in *.c *.h; do
	case " $value " in *" $f "*) continue ;; esac
	grep -q '#include "nocopy_value_int.h"' "$f" && no "$f includes nocopy_value_int.h: use nocopy_value.h"
done
grep -nE '\b(e|pe|safe_e)(m|c|re)alloc|\b(p?efree|estrdup|zend_string_(init|alloc|tolower|concat|dup|release)|smart_str_[a-z]+|zend_new_array|array_init|zend_hash_(add|update|index_add|index_update|str_add|str_update|next_index_insert|init|copy|destroy|extend|rehash)[a-z_]*|zend_call_[a-z_]+|nc_call_magic|zend_throw_[a-z_]+|php_error_docref|zend_error|zval_ptr_dtor|OBJ_RELEASE|object_init_ex|zend_lookup_class|ZVAL_STRINGL?)\b' nocopy_write.c \
	| sed 's/^/check-layers: nocopy_write.c runs under a shard lock (no allocation, no user code): /' | grep . && bad=1
# Kept copies (nocopy_kept.h): no module state either, the store passes its cache and env.
for f in nocopy_kept.c nocopy_kept.h; do
	grep -nE '#include "nocopy_php\.h"|\bNOCOPY_(G|SHARED)\b' "$f" | sed "s|^|check-layers: $f: module state in the kept copies: |" | grep . && bad=1
done
# Above the value layer: no name of nocopy_value_int.h (nocopy.c's one translation unit would let them through).
outer="nocopy.c nocopy_store.c nocopy_kept.c nocopy_kept.h nocopy_resp.c nocopy_resp.h nocopy_php.h"
int=$( { sed -n 's/^#define \([A-Za-z_0-9]*\).*/\1/p' nocopy_value_int.h; sed -n 's/^}[ ]*\([a-z_0-9]*\);.*/\1/p;s/^typedef .*} *\([a-z_0-9]*\);.*/\1/p' nocopy_value_int.h
	grep -E '^[a-z]' nocopy_value_int.h | grep -v '^typedef' | sed -n 's/^[^(]*[^a-z_0-9]\([a-z_0-9]*\)(.*/\1/p'; } | grep -v '^NOCOPY_VALUE_INT_H$' | grep . | paste -sd'|' -)
for f in $outer; do
	[ -f "$f" ] && grep -nwE "$int" "$f" | sed "s|^|check-layers: $f: value layer internal: |" | grep . && bad=1
done
# The arena's data operations: the store's (and the RESP tier's nocopy_clear), not the module's.
data='\bnocopy_(find(_ex)?|each|borrow_(begin|end)|keep|count|put_(begin|commit|abort)|delete|update|clear|lease_(take|drop)|atom_[a-z_]+)\('
grep -nE "$data" nocopy.c | sed 's/^/check-layers: nocopy.c: arena data through the store: /' | grep . && bad=1
grep -nE "$data" nocopy_resp.c | grep -v '\bnocopy_clear()' | sed 's/^/check-layers: nocopy_resp.c: local copies through the store: /' | grep . && bad=1
# The RESP tier: the store reaches it through nocopy_detour, the module through nocopy_resp.h's calls, and nobody else at all.
for f in *.c *.h; do
	case $f in
	nocopy_resp.c | nocopy_resp.h) continue ;;
	nocopy.c) ok='nocopy_vk_(minit|mshutdown|route|new_request|close|listener)\b|\bnocopy_vk\b' ;;
	nocopy_php.h) ok='\bnocopy_vk(_shared)?\b' ;;
	*) ok='^$' ;;
	esac
	grep -noE '\bnocopy_vk[a-z_]*\b|\bHAVE_NOCOPY_RESP\b' "$f" | grep -vE "$ok" | sed "s|^|check-layers: $f: RESP outside nocopy_resp.h: |" | grep . && bad=1
	[ "$f" != nocopy_php.h ] && grep -q '#include "nocopy_resp.h"' "$f" && no "$f includes nocopy_resp.h: nocopy_php.h does"
done
# The local store's pieces are the RESP tier's.
for f in *.c; do
	case $f in nocopy_store.c | nocopy_resp.c) continue ;; esac
	grep -nE '\bnocopy_((put|fetch|drop)_local|store_each)\b' "$f" | sed "s|^|check-layers: $f: the local store is the RESP tier's: |" | grep . && bad=1
done
[ $bad = 0 ] && echo "check-layers: ok"
exit $bad
