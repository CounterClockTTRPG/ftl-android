#!/bin/bash
# Builds the glibc-side stand-ins inside the cross container: libftlhost.so with everything,
# plus libX11.so.6 / libXi.so.6 / libGL.so.1 / libasound.so.2 that just pull it in.
set -euo pipefail
cd "$(dirname "$0")/.."
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" ftl-android-cross bash -euc '
M=out/mesa/opt/mesa
O=out/shim
mkdir -p $O
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter -Ishim -I$M/include"
$CC $CFLAGS -shared -o $O/libftlhost.so shim/host.c shim/x11.c shim/glx.c shim/alsa.c \
    -Wl,-soname,libftlhost.so -L$M/lib -lEGL -lpthread -Wl,-rpath-link,$M/lib -Wl,--no-undefined
echo "" > $O/empty.c
# The rest are only there because box64 lists them as dependencies of the wrapped ones.
for lib in libX11.so.6 libXi.so.6 libGL.so.1 libasound.so.2 libxcb.so.1 libXext.so.6 libXau.so.6 libXdmcp.so.6; do
    $CC -shared -fPIC -o $O/$lib $O/empty.c -Wl,-soname,$lib -Wl,--no-as-needed -L$O -lftlhost
done
rm $O/empty.c
'
ls -la out/shim
