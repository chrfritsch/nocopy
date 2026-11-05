#!/bin/sh
# Replaces third_party/libvalkey with the given libvalkey tag: the sync client and TLS, unchanged. Review the diff, then `./dev ci`.
# Usage: scripts/vendor-libvalkey.sh <tag>
set -eu
TAG=${1:?usage: scripts/vendor-libvalkey.sh <tag>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DST=$ROOT/third_party/libvalkey
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/valkey-io/libvalkey "$TMP/lv"
# keep in step with config.m4
SRC="adlist alloc async conn dict net read sds sockcompat tls valkey"
rm -rf "$DST" && mkdir -p "$DST/include/valkey" "$DST/src"
cp "$TMP/lv/COPYING" "$DST/"
cp "$TMP/lv/include/valkey/"*.h "$DST/include/valkey/"
cp "$TMP/lv/src/"*.h "$DST/src/"
for f in $SRC; do cp "$TMP/lv/src/$f.c" "$DST/src/"; done
echo "$TAG $(git -C "$TMP/lv" rev-parse HEAD)" > "$DST/VERSION"
echo "libvalkey $TAG vendored; upstream changes: https://github.com/valkey-io/libvalkey/blob/$TAG/CHANGELOG.md"
