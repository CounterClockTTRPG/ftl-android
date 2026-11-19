# Third-party components in the APK

The APK bundles these components, unmodified unless noted. Their sources are available at the
locations given; the exact versions are pinned in `scripts/build-runtime.sh` and
`docker/Dockerfile`.

| Component | Version | Licence | Source |
|--|--|--|--|
| box64 | git 5d627690 (2026-11-19) | MIT | https://github.com/ptitSeb/box64 |
| Mesa (Turnip Vulkan, Zink, EGL) | git 8bbd46a1 (27.0.0-devel), with `patches/mesa-zink-kgsl-gpu.patch` | MIT (mostly; see Mesa's docs/license.rst) | https://gitlab.freedesktop.org/mesa/mesa |
| GNU C Library (ld.so, libc, libm, libpthread, libdl, librt, libresolv) | Debian 2.41-12+deb13u4 | LGPL-2.1-or-later | https://sources.debian.org/src/glibc/2.41-12+deb13u4/ |
| libstdc++, libgcc_s | Debian GCC 14.2.0-19 | GPL-3.0 with the GCC Runtime Library Exception | https://sources.debian.org/src/gcc-14/14.2.0-19/ |
| Vulkan loader | Debian 1.4.309.0-1 | Apache-2.0 | https://sources.debian.org/src/vulkan-loader/1.4.309.0-1/ |
| Expat | Debian 2.9.0-1~deb13u1 | MIT | https://sources.debian.org/src/expat/ |
| zlib | Debian 1:1.3.dfsg+really1.3.1-1+deb13u1 | zlib | https://sources.debian.org/src/zlib/ |
| Zstandard | Debian 1.5.7+dfsg-1 | BSD-3-Clause | https://sources.debian.org/src/libzstd/1.5.7+dfsg-1/ |

The glibc libraries are dynamically loaded shared objects and can be replaced: the runtime files
are linked under their real names in the app's `files/rt` directory at start-up (see
`MainActivity.prepareRuntime`), and the build recipe for every piece is in this repository.

**FTL: Faster Than Light** is © Subset Games and is not included; `libsteam_api.so` comes from
the user's own copy of the game.
