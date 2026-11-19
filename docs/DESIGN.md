# FTL for Android -- design

Runs the user's own Linux build of FTL (`FTL.amd64` + `ftl.dat` from Steam depot 221002 or the
GOG Linux installer) unchanged on an arm64 Android device. Nothing of Subset's is in the APK; the
user imports their files at first launch.

## Why this shape

- `FTL.amd64` is x86-64, glibc, and does its own windowing: libX11 + GLX 1.3 (`libGL.so.1`), ALSA
  (`libasound.so.2`), and dlopens libXi (XInput 2.2), libXrandr, libXinerama, libXxf86vm (the
  last three may be missing). No SDL. Every gl* function is fetched with `glXGetProcAddress`.
- box64's bionic build can't run glibc programs (box64 docs/COMPILE.md), so the game process is
  a glibc world: an aarch64 glibc runtime (Debian trixie) + box64 built against it, started
  through `ld-linux-aarch64.so.1`, which is packaged in the APK's native library dir (the only
  place an app may `execve` from since targetSdk 29).
- Android's EGL/GLES drivers are bionic and can't load into a glibc process. Mesa's Turnip
  (Vulkan, talks to `/dev/kgsl-3d0` directly; knows Adreno 810) + Zink (GL on Vulkan) run in
  glibc and give the game desktop GL 4.6 compatibility.
- No X server. Small stand-ins with the real library names implement what FTL calls; box64
  loads them as the "native" libraries it wraps.

## Pieces

| Piece | World | Job |
|--|--|--|
| `app/java` | Android (Java) | file import, runtime links, spawns the game, touch -> mouse gestures, keys |
| `app/jni/ftlview.c` -> `libftlview.so` | bionic | socket server, frames onto the Surface, audio ring -> AAudio |
| `app/jni/launch.c` -> `libgl_launch.so` | bionic executable | runs the game under ptrace; seccomp-trapped calls return ENOSYS |
| `shim/x11.c` -> `libX11.so.6`, `libXi.so.6` | glibc | one display/screen/window, event queue fed by the app, the pointer patch |
| `shim/glx.c` -> `libGL.so.1` | glibc | GLX on Mesa EGL (surfaceless, pbuffer); `glXSwapBuffers` reads the frame into shared memory, paces to 60 Hz |
| `shim/alsa.c` -> `libasound.so.2` | glibc | the snd_pcm_* subset FTL uses, into a shared ring |
| `shim/host.c` | glibc | the link to the app (or headless: frame dumps, test input from a file) |
| box64, Mesa, glibc | glibc | `scripts/build-runtime.sh` |

App <-> game: one abstract unix socket (`shim/host.h`: config, input, frame notices; fds passed
with SCM_RIGHTS) and memfd shared memory (3 frame buffers, an audio ring). The app sets the window
buffer to the game's 1280x720 and the compositor scales it into a centred 16:9 view.

Runtime files are packaged as `lib/arm64-v8a/libgl_<name>.so` (the installer only extracts
`lib*.so`); at start-up the app links them under their real names in `files/rt` (`assets/runtime.txt`).

## Things that weren't obvious

- **Seccomp.** Android's app filter traps `set_robust_list` (99), which glibc's ld.so calls for
  every program before any user code -- so a preloaded SIGSYS handler is too late. `launch.c`
  traces the game, catches SIGSYS stops, sets x0 = -ENOSYS and resumes. `rseq` is turned off with
  `GLIBC_TUNABLES=glibc.pthread.rseq=0`. `adb shell` is not filtered, so this only shows in the app.
- **Zink without DRM.** EGL surfaceless only reaches Zink through its software path (no render
  node on KGSL), where Zink insists on a CPU Vulkan device; `patches/mesa-zink-kgsl-gpu.patch`
  takes the only device instead. Needs `LIBGL_ALWAYS_SOFTWARE=1` + `MESA_LOADER_DRIVER_OVERRIDE=zink`
  (set by `glx.c`).
- **box64's wrappers** expect `_XLockMutex_fn`/`_XUnlockMutex_fn` data symbols in libX11 and list
  libxcb/libXext/libXau/libXdmcp as dependencies (empty libraries satisfy them), and pass some
  `Display` fields through, so the stand-in uses Xlib's real structure layouts.
- **Touch.** The game turns on XInput 2 when the server has XI >= 2.2 and a master device with a
  direct-touch class; the Linux UI then decodes touches but never acts on them (the iPad touch UI
  isn't wired up in this build). Worse, it only records XI's opcode after an inotify watch on
  `/dev/input` succeeds, which an app can't do -- so all XI events were dropped in the app (but
  not from `adb shell`, which may watch it). So: no touch class (`FTLHOST_XI_TOUCH=1` brings it
  back), core pointer events, and the app turns touches into mouse gestures.
- **Clicks need frames.** FTL samples the mouse once per frame, so press and release in the same
  frame are lost: `x11.c` sends each button event at least two presented frames after the
  previous pointer event.
- **The pointer.** `MouseControl::OnRender` skips the pointer images (not the tooltips) when its
  hide flag is set; `x11.c` turns that `jne` at 0x53c6ab into a `jmp` after checking the bytes.

## Status

Done: import, rendering, audio, touch gestures, keyboard, hidden pointer. Not done: GPUs other
than Adreno, pausing the game process when the app is in the background (audio stops; the game
keeps running), a gamepad, the GOG `.sh` installer as an import source.
