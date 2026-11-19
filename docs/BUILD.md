# Building

Linux x86-64 host. Needs:

- **Docker** (the arm64 glibc side is cross-compiled in `docker/Dockerfile`, Debian trixie).
- **Android SDK** with `platforms;android-36`, `build-tools;36.0.0`, `platform-tools` and an
  **NDK** (r30 used): `sdkmanager "ndk;30.0.16248370"`. Default location `~/Android/sdk`
  (`ANDROID_HOME` overrides).
- **JDK 17+** (`javac`, `keytool`) and Python 3.

No Gradle: `scripts/build-apk.sh` drives aapt2, d8, zipalign and apksigner directly.

```sh
scripts/build-runtime.sh   # box64, Mesa, glibc runtime, shim -> out/  (first run ~10 min)
scripts/build-apk.sh       # -> out/ftlport.apk, signed with ~/.android/debug.keystore
DEBUG=1 scripts/build-apk.sh   # debuggable build (run-as, debugging hooks)
adb install -r out/ftlport.apk
```

`scripts/build-shim.sh` rebuilds just the stand-in libraries after a change in `shim/`.

## Debugging on a device

With a `DEBUG=1` build, files in the app's storage switch on hooks (`adb shell run-as local.ftlport ...`):

- `files/trace`: logs every touch and X event (logcat tag `ftlport`), reads test input from
  `files/input` and dumps frames to `files/dump/` on request. Input commands, one per line:
  `mmove X Y`, `mdown B X Y`, `mup B X Y`, `click X Y`, `key HEXKEYSYM`, `dump`, `close`.
- `files/cmdline.txt`: runs this command instead of the game, one argument per line (`$LIB` is
  the native library dir, `$RT` the runtime links), in the app's real sandbox.
- `files/nosocket`: runs the game without connecting to the app.

`scripts/device/run-headless.sh` runs the game from `adb shell` without the app (push the runtime
to `/data/local/tmp/ftl`, see the script). Note that `adb shell` is not the app sandbox: it may
watch `/dev/input` and isn't under seccomp, so confirm anything input- or syscall-related in the app.
