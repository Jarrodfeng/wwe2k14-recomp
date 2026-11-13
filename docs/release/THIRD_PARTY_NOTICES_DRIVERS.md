# Third-party notices: bundled graphics drivers

This file is added to the Android app only by a build made with `tools\android\build_apk.ps1 -BundleDrivers`. The standard
(release) APK packs no drivers and does not include it. The licence texts are the app's `LICENSE_mesa.txt` and
`LICENSE_mali-panvk.txt` assets, added by the same switch.

| Library | In | Licence |
|---|---|---|
| Mesa 26.2.5: Turnip, the Adreno Vulkan driver (built unmodified from https://gitlab.freedesktop.org/mesa/mesa) | libvulkan_freedreno.so | MIT (the Mesa 3D Graphics Library authors, as listed in each source file); some files under other permissive licences, each named in its header |
| mali-panvk (https://github.com/JimVulkan/mali-panvk, commit d896b0a): Mesa's PanVK Mali Vulkan driver with a kbase backend, built unmodified | libvulkan_panfrost.so | MIT. Copyright (c) 2026 JimVulkan; Copyright (c) The Mesa 3D Graphics Library authors, as listed in each source file |
