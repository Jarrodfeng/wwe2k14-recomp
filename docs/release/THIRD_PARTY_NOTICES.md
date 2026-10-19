# Third-party notices

This program uses the libraries below. Each keeps its own licence; the full licence texts follow this table
(added by `tools\release\make_release.ps1` from each library's own licence file). Inventory: Session 25, from the
link lines of the release build (`rexruntime.dll`, `rexgpu-xenos.dll`) and the build files.

| Library | In | Licence |
|---|---|---|
| ReXGlue SDK (rexruntime.dll, rexgpu-xenos.dll; with this project's patches), portions from Xenia | the two DLLs | BSD 3-clause. Copyright (c) 2026 Tom Clay; portions (c) 2022 Ben Vanik and Xenia contributors |
| SDL 3.5.0 | rexruntime.dll (static) | zlib. Copyright (C) 1997-2026 Sam Lantinga |
| FFmpeg 4.4 (libavcodec, libavutil; audio decoding) | rexruntime.dll (static) | **LGPL 2.1 or later** (see below) |
| libmspack (lzxd.c: LZX decompression) | rexruntime.dll (static) | **LGPL 2.1** (see below). (C) 2003-2023 Stuart Caie |
| Dear ImGui 1.92.5 | rexruntime.dll, wwe2k14_launcher.exe | MIT. Copyright (c) 2014-2025 Omar Cornut |
| {fmt} 12.1.0 | both DLLs | MIT. Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors |
| spdlog 1.17.0 | both DLLs | MIT. Copyright (c) 2016 - present, Gabi Melman and spdlog contributors |
| xxHash 0.8 | both DLLs | BSD 2-clause. Copyright (c) 2012-2021 Yann Collet |
| glslang | rexgpu-xenos.dll | BSD 3-clause / BSD 2-clause / MIT / Apache 2.0 and the NVIDIA 2002 preprocessor notice (all in its licence file) |
| Vulkan Memory Allocator | both DLLs (header-only) | MIT. Copyright (c) 2017-2025 Advanced Micro Devices, Inc. |
| Vulkan-Headers | header-only | Apache 2.0 or MIT. Copyright 2015-2026 The Khronos Group Inc. |
| SPIRV-Headers | header-only | MIT-style. Copyright (c) 2015-2024 The Khronos Group Inc. |
| aes_128 | rexruntime.dll | MIT. Copyright (c) 2017 LuoPeng |
| o1heap | rexruntime.dll | MIT. Copyright (c) 2020 Pavel Kirienko |
| toml++ | header-only | MIT. Copyright (c) Mark Gillard |
| SIMDe 0.8.2 | header-only | MIT. Copyright (c) 2017 Evan Nemerson |
| utfcpp | header-only | Boost Software License 1.0 |
| stb_image 2.29 | rexruntime.dll, wwe2k14.exe, wwe2k14_launcher.exe | MIT or public domain. Copyright (c) 2017 Sean Barrett |
| nlohmann/json 3.11.3 | wwe2k14_launcher.exe | MIT. Copyright (c) 2013-2021 Niels Lohmann |
| NVIDIA Streamline 2.14.1 headers | rexruntime.dll | MIT. Copyright (c) 2022-2023 NVIDIA CORPORATION |
| DXBCChecksum (derived from the RSA Data Security, Inc. MD5 Message-Digest Algorithm) | rexgpu-xenos.dll | Notice in its source: Copyright 2008-2016 (c) Advanced Micro Devices, Inc.; Copyright (C) 1990, RSA Data Security, Inc. |
| RenderDoc API header | header-only | MIT. Copyright (c) 2015-2016 Baldur Karlsson |
| DXC API headers | header-only | University of Illinois/NCSA. Copyright (C) Microsoft Corporation |
| disruptorplus | header-only | licence not found in the source tree (see the release notes for Jarrod) |
| ProggyTiny font (embedded in ReXGlue's overlay) | rexruntime.dll | licence not in the source tree (see the release notes for Jarrod) |

## LGPL components (FFmpeg, libmspack)

FFmpeg (libavcodec / libavutil 4.4) and libmspack's LZX decoder are statically linked into **`rexruntime.dll`**, a
separate DLL that `wwe2k14.exe` loads at run time: you can replace it with your own build. Their source code:

- FFmpeg 4.4, the fork used by the ReXGlue SDK: https://github.com/wmarti/FFmpeg (branch `xenia-ffmpeg-canary-full`).
- libmspack: https://www.cabextract.org.uk/libmspack/
- The ReXGlue SDK that builds `rexruntime.dll`: https://github.com/rexglue/rexglue-sdk, plus this project's patches.

The GNU Lesser General Public License 2.1 text follows below.

## Included as separate files

- **NVIDIA DLSS and Streamline** (`nvidia\`):
  - Streamline (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`) is under the MIT licence, Copyright (c) 2022-2023
    NVIDIA CORPORATION (`nvidia\streamline.license.txt`).
  - `nvngx_dlss.dll` is under NVIDIA's RTX SDK licence (`nvidia\nvngx_dlss.license.txt`). End users may use it only
    with this program, and may not reverse engineer, decompile or disassemble it, or distribute it on its own.
  - NVIDIA, the NVIDIA logo, DLSS and RTX are trademarks and/or registered trademarks of NVIDIA Corporation.
- **AMD FidelityFX** (`amd_fidelityfx_vk.dll`, FSR 3 upscaling): built unmodified from AMD's FidelityFX SDK 1.1.4
  source (https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, its ffx-api with the Vulkan backend), MIT
  licence, Copyright (C) 2024 Advanced Micro Devices, Inc. (licence text below). AMD, FidelityFX and FSR are
  trademarks of Advanced Micro Devices, Inc.
- **The Microsoft Visual C++ runtime** (`msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`,
  `vcruntime140_1.dll`): Microsoft's redistributable files from Visual Studio 2022, distributed unmodified under the
  Visual Studio licence's Distributable Code terms.

## Not included

- No game files.

---

# Licence texts
