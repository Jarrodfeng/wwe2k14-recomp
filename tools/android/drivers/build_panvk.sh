#!/bin/sh
# Session 72 part 7: PanVK for Mali on stock Android (kbase), with the project's own two-stage build.sh (host tools,
# then the driver). Runs inside the PanVK container (Dockerfile.panvk): /src = thirdparty\mali-panvk (read-only),
# /out = out\android\drivers. GRALLOC=0: no IMapper backend (its README: a loader that keeps the driver in the app's own
# linker namespace, as libadrenotools does, may not let it see the platform libraries); Arm's and MediaTek's buffer
# handles are read directly instead. Meson fetches libdrm / expat / zlib / perfetto on the first configure (network).
set -eu
rm -rf /tmp/panvk && cp -a /src /tmp/panvk
# Symbolic links from the Windows checkout come back with Linux git (as for Mesa).
git -C /tmp/panvk -c safe.directory='*' -c core.symlinks=true checkout -f HEAD -- .
cd /tmp/panvk
GRALLOC=0 sh ./build.sh "$NDK"
mkdir -p /out
cp build-android/libvulkan_panfrost.so /out/libvulkan_panfrost.so
file /out/libvulkan_panfrost.so
ls -la /out/libvulkan_panfrost.so
