#!/bin/sh
# Editor loop: edit form, save (POST), view, /admin/content, add form. Usage: editor.sh BASE COOKIEJAR OUT "NIDS"
# OUT gets "step<TAB>seconds" lines; exits 1 unless every save returned 303.
B=$1; CK=$2; OUT=$3; NIDS=$4
: > "$OUT"; bad=0
t() { curl -s -o /dev/null -b "$CK" -w '%{time_total}' "$@"; }
for n in $NIDS; do
  page=$(curl -s -b "$CK" -w '\n%{time_total}' "$B/node/$n/edit")
  printf 'edit\t%s\n' "$(echo "$page" | tail -1)" >> "$OUT"
  fbid=$(echo "$page" | grep -o 'name="form_build_id" value="[^"]*"' | head -1 | sed 's/.*value="//;s/"//')
  tok=$(echo "$page" | grep -o 'name="form_token" value="[^"]*"' | head -1 | sed 's/.*value="//;s/"//')
  r=$(curl -s -o /dev/null -b "$CK" -w '%{http_code} %{time_total}' --data-urlencode "title[0][value]=Edited $n $$-$(date +%s%N)" \
    --data-urlencode "body[0][value]=<p>Body $(date +%s%N)</p>" --data-urlencode "body[0][format]=basic_html" \
    --data-urlencode "status[value]=1" --data-urlencode "form_build_id=$fbid" --data-urlencode "form_token=$tok" \
    --data-urlencode "form_id=node_article_edit_form" --data-urlencode "op=Save" "$B/node/$n/edit")
  [ "${r% *}" = 303 ] || { echo "save of node $n returned ${r% *}" >&2; bad=1; }
  printf 'save\t%s\n' "${r#* }" >> "$OUT"
  printf 'view\t%s\n' "$(t "$B/node/$n")" >> "$OUT"
  printf 'list\t%s\n' "$(t "$B/admin/content")" >> "$OUT"
  printf 'add\t%s\n' "$(t "$B/node/add/article")" >> "$OUT"
done
exit $bad
