# Cross-compile container: everything that runs in the game's glibc (aarch64) world.
FROM debian:trixie
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
      crossbuild-essential-arm64 cmake ninja-build meson pkg-config git ca-certificates python3 \
      python3-mako python3-yaml python3-packaging python3-ply bison flex glslang-tools patchelf file \
      libexpat1-dev:arm64 zlib1g-dev:arm64 libzstd-dev:arm64 libdrm-dev:arm64 \
      libx11-dev:arm64 libxi-dev:arm64 libxrandr-dev:arm64 libxinerama-dev:arm64 libasound2-dev:arm64 libvulkan-dev:arm64 libegl-dev:arm64 libgl-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
COPY cross-aarch64.txt /opt/cross-aarch64.txt
WORKDIR /src
