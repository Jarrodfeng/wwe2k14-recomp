#!/bin/sh
# Session 72 part 7: Turnip (Mesa's Adreno Vulkan driver) for stock Android, as Mesa's docs\android.rst builds it
# ("cross-compiling the turnip driver for a stock Pixel phone"). Runs inside the container (Dockerfile here):
#   /src = thirdparty\mesa (read-only), /out = out\android\drivers. API level 28 = the app's minSdkVersion.
set -eu
API=28
B=/tmp/build-turnip
mkdir -p /out
cat > /tmp/android-aarch64 <<EOF
[constants]
ndk_path = '$NDK'
[binaries]
ar = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
c = [ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang']
cpp = [ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
nm = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
rm -rf "$B"
rm -rf /tmp/mesa && cp -a /src /tmp/mesa  # a writable copy: meson writes into subprojects/ when it looks for a fallback
# Mesa has symbolic links (include/android_stub/...); the Windows checkout holds them as text files: Linux git restores them.
git -C /tmp/mesa -c safe.directory='*' -c core.symlinks=true checkout -f HEAD -- .
# Session 73: our backports on top of the tag (mesa_patches\*.patch: upstream Mesa's Adreno 710 / 720 / 722 entries).
for p in /scripts/mesa_patches/*.patch; do
  [ -e "$p" ] || continue
  git -C /tmp/mesa -c safe.directory='*' apply "$p" || exit 1
  echo "applied $(basename "$p")"
done
meson setup "$B" /tmp/mesa --cross-file /tmp/android-aarch64 \
  -Dbuildtype=release -Dstrip=true -Db_ndebug=true \
  -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Degl=disabled -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Dxmlconfig=disabled -Dexpat=disabled -Dzstd=disabled --wrap-mode=nodownload
meson compile -C "$B"
cp "$B/src/freedreno/vulkan/libvulkan_freedreno.so" /out/libvulkan_freedreno.so
file /out/libvulkan_freedreno.so
ls -la /out/libvulkan_freedreno.so
