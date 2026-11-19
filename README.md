# FTL for Android (unofficial)

Runs the **Linux version of FTL: Faster Than Light** (Advanced Edition, 1.6.13) on arm64 Android
phones and tablets, with touch controls. This is a compatibility layer, like the
[PortMaster port](https://github.com/PortsMaster/PortMaster-New/tree/main/ports/ftl) it was inspired
by: the game itself is **not included**. You need your own copy of FTL from Steam or GOG.

Not affiliated with or endorsed by [Subset Games](https://subsetgames.com/). FTL: Faster Than
Light is © Subset Games.

## Status

Tested on a POCO M8 Pro 5G (Snapdragon 7s Gen 3, Adreno 810, HyperOS 3 / Android 16): plays
start to finish at 60 fps with sound and touch.

- **GPU**: Qualcomm Adreno only (Mesa's Turnip driver talks to the GPU directly). Other GPUs
  (Mali, PowerVR, Xclipse) are not supported yet.
- **Android**: 10 or newer, arm64.
- **Game files**: the Linux build (`FTL.amd64`, `ftl.dat`, `amd64/libsteam_api.so`).

## Getting the game files

The app imports a **zip** that contains the three files above, wherever they are in it.

- **Steam** (any OS): open `steam://open/console` in a browser and enter
  `download_depot 212680 221002`. Zip the `data` folder of the downloaded depot
  (`Steam/steamapps/content/app_212680/depot_221002/`).
- **GOG**: from the Linux installer, zip the `data/noarch/game/data` folder.

Copy the zip to the device, install the APK from [Releases](../../releases), open **FTL Port**,
and pick the zip. The files are copied into the app's private storage; the zip can be deleted
afterwards. Saves live there too (`files/home/.local/share/FasterThanLight`).

## Controls

| Touch | Does |
|--|--|
| Tap | Left click |
| Hold still ~0.3 s | Right click (vibrates) |
| Quick flick **up** | Left click where the flick started: add power to a system or weapon |
| Quick flick **down** | Right click where the flick started: remove power |
| Drag (slow, or sideways) | Left drag: crew selection box, moving items |
| Two-finger tap | Pause / resume (space) |
| Three-finger tap | On-screen keyboard (ship names, hotkeys) |
| Back gesture | Escape (in-game menu) |

A physical keyboard and mouse work as they do on desktop. The game's own mouse pointer is hidden.

## How it works

```
Android app (Java + JNI)                    game process (glibc world, separate)
  SurfaceView  <-- frames (shared memory) --   libGL stand-in: GLX on Mesa EGL, Zink -> Turnip -> Adreno
  AAudio       <-- audio ring (shared mem) --  libasound stand-in
  touch -> mouse gestures --- unix socket -->  libX11/libXi stand-in: one display, one window
                                                     ^
                                        box64 runs the unmodified FTL.amd64 (x86-64)
```

- **box64** translates the x86-64 game to arm64. It needs glibc, so the app ships a small glibc
  runtime (Debian trixie arm64) and starts it through `ld-linux-aarch64.so.1`.
- **No X server**: the game uses ~90 Xlib calls, 13 GLX and ~40 ALSA functions; the `shim/`
  libraries implement just those, under the real library names, and talk to the app.
- **Graphics**: Android's GL drivers can't be loaded into a glibc process, so the game renders
  with Mesa (Zink on Turnip, desktop OpenGL 4.6) straight on the GPU, and each frame is copied
  to the app.
- **Sandbox**: Android's app seccomp filter kills glibc's loader (`set_robust_list`); a small
  launcher runs the game under ptrace and turns such calls into `ENOSYS`.
- **Pointer**: the game's pointer drawing is skipped by changing one jump in memory at start-up,
  only after checking the bytes match FTL 1.6.13; files on disk are never modified.

The details, and what was learned along the way, are in [docs/DESIGN.md](docs/DESIGN.md).

## Building

See [docs/BUILD.md](docs/BUILD.md): Docker for the arm64 glibc side, the Android SDK + NDK for
the app, no Gradle.

## Licence

The code in this repository is under the [MIT licence](LICENSE). The APK also contains box64,
Mesa, glibc and other components under their own licences: see
[THIRD_PARTY.md](THIRD_PARTY.md).
