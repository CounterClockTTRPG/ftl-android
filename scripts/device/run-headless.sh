#!/system/bin/sh
# Runs FTL from adb shell without the app: frames go to dump/, test input is read from input.
# Layout under /data/local/tmp/ftl: game/ (FTL.amd64, ftl.dat, amd64/), rt/ (the runtime under
# its real names, as the app links it: box64, ld-linux-aarch64.so.1, lib*.so*, freedreno_icd.json
# pointing at rt/libvulkan_freedreno.so), home/, cache/, dump/.
# Not the app sandbox: no seccomp, and the shell user may watch /dev/input.
R=/data/local/tmp/ftl
cd $R/game
touch $R/input
mkdir -p $R/home/.local/share/FasterThanLight $R/cache $R/dump
export HOME=$R/home XDG_DATA_HOME=$R/home/.local/share XDG_CACHE_HOME=$R/cache DISPLAY=:0 LC_ALL= LC_NUMERIC=C
export FTLHOST_TRACE=${TRACE:-} FTLHOST_DUMP=$R/dump FTLHOST_INPUT=$R/input FTLHOST_DUMP_EVERY=${EVERY:-120}
export VK_ICD_FILENAMES=$R/rt/freedreno_icd.json BOX64_LD_LIBRARY_PATH=$R/game/amd64 BOX64_LIBGL=$R/rt/libGL.so.1
export BOX64_LOG=${BLOG:-1} BOX64_NOBANNER=1 GLIBC_TUNABLES=glibc.pthread.rseq=0
exec $R/rt/ld-linux-aarch64.so.1 --library-path $R/rt $R/rt/box64 ./FTL.amd64 "$@"
