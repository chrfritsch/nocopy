#!/bin/sh
# Inside the image: clang-tidy (checks in /src/.clang-tidy) over the extension sources; needs /build/config.h from a build.
set -e
/src/docker/build.sh
cd /src && ls *.c arena/*.c | xargs -P"$(nproc)" -I{} clang-tidy --quiet {} -- -I/build $(php-config --includes) -DHAVE_CONFIG_H -DZEND_COMPILE_DL_EXT=1 -D_GNU_SOURCE -Ithird_party/libvalkey/include -DVALKEY_VISIBILITY_H -DLIBVALKEY_API=
