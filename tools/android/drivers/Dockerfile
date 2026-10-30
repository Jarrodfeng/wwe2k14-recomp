# Session 72 part 7 (Jarrod: "we should have many drivers for all the most common android devices" -> "2": bundled
# drivers built from source): the Linux build box for Mesa's Android Vulkan drivers (Turnip for Adreno, PanVK for Mali).
# Mesa's own recipe (thirdparty\mesa\docs\android.rst) cross-compiles with the Linux NDK, so it runs in a container
# instead of on the PC. Everything comes from official sources: Ubuntu's packages, PyPI's meson, Google's NDK.
#   built and run by tools\android\drivers\build_drivers.ps1
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-pip python3-mako python3-yaml python3-packaging python3-ply \
      bison flex glslang-tools ninja-build pkg-config cmake git wget unzip ca-certificates file \
    && rm -rf /var/lib/apt/lists/*
# Mesa 26.2 needs meson >= 1.4 (Ubuntu 24.04 has 1.3).
RUN pip3 install --break-system-packages 'meson==1.9.1'
# The NDK the game uses (29.0.14206865 = r29), Linux build.
ARG NDK=android-ndk-r29
RUN wget -q https://dl.google.com/android/repository/${NDK}-linux.zip -O /tmp/ndk.zip \
    && unzip -q /tmp/ndk.zip -d /opt && rm /tmp/ndk.zip && mv /opt/${NDK} /opt/ndk
ENV NDK=/opt/ndk
