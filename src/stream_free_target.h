// S105 A1b: does the exe this launcher starts (the game, or the game library of the same APK) have no command stream? The release build is the
// stream-free build (DECISIONS 2026-11-18 20:41 / 20:50), and the launcher is built by the SAME configure as the game exe (CMakeLists.txt passes
// WWE2K14_KEEP_STREAM to wwe2k14_launcher too; src/launcher_mobile/CMakeLists.txt takes it from build_launcher.ps1 -StreamFree, which build_all.ps1 passes
// with the game library's -StreamFree). A target that does not pass it (fview, the unit rigs, a dev launcher) gets the stream build, where every setting works.
// What the stream-free build cannot do (DLSS / DLAA / FSR, PREPARE SHADERS, the advanced renderer flag) is hidden behind this constant; the lint
// tools\launcher\settings_reader_lint.py checks each hidden control sits behind it.
#pragma once

#include "keep_stream.h"

namespace wwe2k14 {
inline constexpr bool kStreamFreeTarget = !WWE2K14_STREAM();
}  // namespace wwe2k14
