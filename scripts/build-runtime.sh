#!/bin/bash
# Builds everything that runs in the game's glibc (aarch64) world, in the cross container:
#   out/stage/box64, out/stage/lib (glibc + friends from Debian trixie arm64), out/mesa (Turnip
#   + Zink + surfaceless EGL), out/shim (the X11/GLX/ALSA stand-ins).
# Sources are fetched into third_party/ at the pinned commits below.
set -euo pipefail
cd "$(dirname "$0")/.."
BOX64_REPO=https://github.com/ptitSeb/box64.git
BOX64_COMMIT=5d627690a3ddee1173bbfeeab066361ba969b158
MESA_REPO=https://gitlab.freedesktop.org/mesa/mesa.git
MESA_COMMIT=8bbd46a1db75304453abfc50d792839da4d58cf5

fetch() {   # dir repo commit
  if [ ! -d "$1/.git" ]; then
    git init -q "$1" && git -C "$1" remote add origin "$2"
  fi
  if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
    git -C "$1" fetch -q --depth 1 origin "$3" && git -C "$1" checkout -q --force FETCH_HEAD
  fi
}
mkdir -p third_party build out
fetch third_party/box64 $BOX64_REPO $BOX64_COMMIT
fetch third_party/mesa $MESA_REPO $MESA_COMMIT
# Zink without a DRM node (Android/KGSL): take the GPU on the software path.
git -C third_party/mesa apply --check ../../patches/mesa-zink-kgsl-gpu.patch 2>/dev/null &&
  git -C third_party/mesa apply ../../patches/mesa-zink-kgsl-gpu.patch

docker build -q -t ftl-android-cross docker > /dev/null
run() { docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" ftl-android-cross bash -euc "$1"; }

echo "== box64"
run 'cmake -S third_party/box64 -B build/box64 -G Ninja -DARM64=1 -DBAD_SIGNAL=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
       -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_ASM_COMPILER=aarch64-linux-gnu-gcc > build/box64.log 2>&1
     ninja -C build/box64 >> build/box64.log 2>&1'

echo "== mesa"
run '[ -f build/mesa/build.ninja ] || meson setup build/mesa third_party/mesa --cross-file /opt/cross-aarch64.txt \
       --prefix=/opt/mesa -Dbuildtype=release -Dplatforms= -Dgallium-drivers=zink -Dvulkan-drivers=freedreno \
       -Dfreedreno-kmds=kgsl -Degl=enabled -Dgbm=disabled -Dglx=disabled -Dglvnd=disabled -Dllvm=disabled \
       -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
       -Dbuild-tests=false -Dzstd=enabled -Dvulkan-layers= -Dtools= > build/mesa-setup.log 2>&1
     ninja -C build/mesa > build/mesa-build.log 2>&1
     DESTDIR=/src/out/mesa ninja -C build/mesa install > /dev/null 2>&1'

echo "== glibc runtime"
run 'rm -rf out/stage && mkdir -p out/stage/lib
     for f in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libresolv.so.2 \
              libgcc_s.so.1 libstdc++.so.6; do cp -L /usr/aarch64-linux-gnu/lib/$f out/stage/lib/; done
     for f in libexpat.so.1 libz.so.1 libzstd.so.1 libvulkan.so.1; do cp -L /usr/lib/aarch64-linux-gnu/$f out/stage/lib/; done
     cp build/box64/box64 out/stage/ && aarch64-linux-gnu-strip out/stage/box64'

echo "== shim"
scripts/build-shim.sh > /dev/null
echo "runtime ready: out/stage out/mesa out/shim"
