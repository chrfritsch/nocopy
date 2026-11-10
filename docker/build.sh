#!/bin/sh
# Inside the image: build /src out of tree into /build/modules/nocopy.so. NOCOPY_CONFIGURE: extra configure flags; NOCOPY_ASAN=1: ASan+UBSan and nocopy's own assertions (NOCOPY_DEBUG).
set -e
# make, not configure: -Werror would break configure's probes
W="-Wall -Wextra -Wno-unused-parameter -Werror"
S= L=
# libtool drops -fsanitize when linking: name the runtimes
[ -n "$NOCOPY_ASAN" ] && S="-O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -DNOCOPY_DEBUG" L="-lasan -lubsan"
rm -rf /build && mkdir /build && cp -R /src/*.c /src/*.h /src/config.m4 /src/arena /src/third_party /build/
cd /build && phpize >/dev/null 2>&1 && ./configure $NOCOPY_CONFIGURE >/dev/null 2>make.err \
  && make -j"$(nproc)" EXTRA_CFLAGS="$W $S" EXTRA_LDFLAGS="$L" >/dev/null 2>>make.err || { cat make.err; exit 1; }
grep -i warning make.err | grep -v shtool || true
