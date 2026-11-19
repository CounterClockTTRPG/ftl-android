#!/bin/bash
# Builds out/ftlport.apk with the SDK tools directly (no Gradle):
# JNI lib (NDK), the glibc-side runtime packaged as lib*.so, Java -> dex, aapt2, sign.
# Needs: out/stage (glibc + box64), out/mesa, out/shim  (see docs/BUILD.md).
set -euo pipefail
cd "$(dirname "$0")/.."
SDK=${ANDROID_HOME:-$HOME/Android/sdk}
BT=$SDK/build-tools/36.0.0
JAR=$SDK/platforms/android-36/android.jar
NDK=$(ls -d $SDK/ndk/* | sort -V | tail -1)
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
B=build/apk
rm -rf $B && mkdir -p $B/lib/arm64-v8a $B/assets $B/classes

# 1. The app's native half
$CLANG -O2 -Wall -fPIC -shared -o $B/lib/arm64-v8a/libftlview.so app/jni/ftlview.c -landroid -laaudio -llog
$CLANG -O2 -Wall -fPIE -pie -o $B/lib/arm64-v8a/libgl_launch.so app/jni/launch.c -llog   # an executable

# 2. The game's runtime, each file as libgl_<name>.so; runtime.txt maps them back
M=out/mesa/opt/mesa/lib
declare -A files=(
  [box64]=out/stage/box64
  [libEGL.so.1]=$M/libEGL.so.1
  [libgallium-27.0.0-devel.so]=$M/libgallium-27.0.0-devel.so
  [libvulkan_freedreno.so]=$M/libvulkan_freedreno.so
)
for f in out/stage/lib/*; do files[$(basename $f)]=$f; done
for f in out/shim/*.so*; do files[$(basename $f)]=$f; done
: > $B/assets/runtime.txt
for name in "${!files[@]}"; do
  if [ "$name" = ld-linux-aarch64.so.1 ]; then pkg=libgl_ld.so
  else pkg=libgl_$(echo "$name" | tr -c 'A-Za-z0-9\n' '_').so; fi
  cp -L "${files[$name]}" $B/lib/arm64-v8a/$pkg
  echo "$pkg $name" >> $B/assets/runtime.txt
done
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" ftl-android-cross bash -c \
  "aarch64-linux-gnu-strip --strip-unneeded $B/lib/arm64-v8a/libgl_*.so 2>/dev/null || true"

# 3. Java
javac -nowarn --release 17 -classpath $JAR -d $B/classes $(find app/java -name '*.java')
$BT/d8 --release --min-api 29 --lib $JAR --output $B $(find $B/classes -name '*.class')

# 4. Package
# DEBUG=1: debuggable (run-as, the files/trace and files/cmdline.txt hooks); off for releases
$BT/aapt2 link -o $B/base.apk --manifest app/AndroidManifest.xml -I $JAR ${DEBUG:+--debug-mode}
python3 -I - $B <<'EOF'
import os, sys, zipfile
b = sys.argv[1]
with zipfile.ZipFile(f'{b}/base.apk', 'a') as z:
    z.write(f'{b}/classes.dex', 'classes.dex')
    z.write(f'{b}/assets/runtime.txt', 'assets/runtime.txt')
    for f in sorted(os.listdir(f'{b}/lib/arm64-v8a')):
        z.write(f'{b}/lib/arm64-v8a/{f}', f'lib/arm64-v8a/{f}', compress_type=zipfile.ZIP_DEFLATED)
EOF
$BT/zipalign -p -f 4 $B/base.apk $B/aligned.apk
$BT/apksigner sign --ks $HOME/.android/debug.keystore --ks-pass pass:android --key-pass pass:android \
  --out ${OUT:-out/ftlport.apk} $B/aligned.apk
ls -la ${OUT:-out/ftlport.apk}
