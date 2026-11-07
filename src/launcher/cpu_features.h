// wwe2k14 launcher - which game exe this CPU runs (Session 95, guest code compile target). No UI code here.
//
// Two exes can sit side by side: wwe2k14.exe (the recompiled game code built for SSE4.1, runs on everything the project
// supports) and wwe2k14_v3.exe (the same game code built for x86-64-v3: AVX2, BMI1/2, FMA, MOVBE, LZCNT, POPCNT, with
// -ffp-contract=off so a*b+c stays unfused and results stay bit-identical; tools\perf\guest_variant_exe.py --variant v3).
// One exe cannot pick per function: inline header functions compiled for v3 can leak through the linker. So the choice is
// made here, before the game starts. An exe chosen in Settings (Game exe) always wins; a package without the v3 exe, or a
// CPU or Windows without every feature, gets wwe2k14.exe. Haswell (2013) and newer have all of it, AMD from Excavator.

#pragma once

#include <intrin.h>

#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace launcher {

// Every feature the v3 exe's code may use: x86-64-v3 as clang's -mavx2 -mfma -mbmi -mbmi2 -mmovbe -mlzcnt -mpopcnt.
inline bool CpuRunsV3() {
  int r[4] = {};
  __cpuid(r, 0);
  if (r[0] < 7) return false;
  __cpuid(r, 1);
  const unsigned ecx1 = unsigned(r[2]);
  const bool fma = ecx1 & (1u << 12), movbe = ecx1 & (1u << 22), popcnt = ecx1 & (1u << 23), osxsave = ecx1 & (1u << 27),
             avx = ecx1 & (1u << 28);
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = unsigned(r[1]);
  const bool bmi1 = ebx7 & (1u << 3), avx2 = ebx7 & (1u << 5), bmi2 = ebx7 & (1u << 8);
  __cpuid(r, int(0x80000000u));
  if (unsigned(r[0]) < 0x80000001u) return false;
  __cpuid(r, int(0x80000001u));
  const bool lzcnt = unsigned(r[2]) & (1u << 5);
  // The YMM registers must also be saved by Windows across thread switches (bit 2 of the enabled state: AVX).
  const bool os_ymm = osxsave && (GetEnabledXStateFeatures() & (1ull << 2));
  return fma && movbe && popcnt && avx && os_ymm && bmi1 && avx2 && bmi2 && lzcnt;
}

// The exe a Play starts when Settings names none.
inline std::filesystem::path PickGameExe(const std::filesystem::path& launcher_dir) {
  std::error_code ec;
  const std::filesystem::path v3 = launcher_dir / "wwe2k14_v3.exe";
  if (std::filesystem::is_regular_file(v3, ec) && CpuRunsV3()) return v3;
  return launcher_dir / "wwe2k14.exe";
}

}  // namespace launcher
