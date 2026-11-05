#!/bin/sh
# Inside the image: build /src out of tree into /build/modules/nocopy.so. NOCOPY_CONFIGURE: extra configure flags.
set -e
rm -rf /build && mkdir /build && cp /src/*.c /src/*.h /src/config.m4 /build/
cd /build && phpize >/dev/null 2>&1 && ./configure $NOCOPY_CONFIGURE >/dev/null 2>make.err && make -j"$(nproc)" >/dev/null 2>>make.err || { cat make.err; exit 1; }
grep -i warning make.err | grep -v shtool || true
