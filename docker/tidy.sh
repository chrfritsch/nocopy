#!/bin/sh
# Inside the image: clang-tidy (checks in /src/.clang-tidy) over the extension sources; needs /build/config.h from a build.
set -e
/src/docker/build.sh
cd /src && clang-tidy --quiet *.c -- -I/build $(php-config --includes) -DHAVE_CONFIG_H -DZEND_COMPILE_DL_EXT=1 -D_GNU_SOURCE
