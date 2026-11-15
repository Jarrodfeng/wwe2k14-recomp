// wwe2k14 launcher - which game exe this CPU runs (Session 95, guest code compile target). No UI code here.
//
// Two exes can sit side by side: wwe2k14.exe (the recompiled game code built for SSE4.1, runs on everything the project
// supports) and wwe2k14_v3.exe (the same game code built for x86-64-v3: AVX2, BMI1/2, FMA, MOVBE, LZCNT, POPCNT, with
// -ffp-contract=off so a*b+c stays unfused and results stay bit-identical; tools\perf\guest_variant_exe.py --variant v3).
// One exe cannot pick per function: inline header functions compiled for v3 can leak through the linker. So the choice is
// made here, before the game starts. An exe chosen in Settings (Game exe) always wins; a package without the v3 exe, or a
// CPU or Windows without every feature, gets wwe2k14.exe. Haswell (2013) and newer have all of it, AMD from Excavator.
// Session 102 W1: the choice itself is in exe_pick.h (tested without a CPU); --wwe2k14_build=avx2|base in the Advanced extra
// flags overrides it (base always; avx2 never on a CPU that cannot run it).

#pragma once

#include <intrin.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>

#include "exe_pick.h"

#pragma comment(lib, "version.lib")

namespace launcher {

// The real CPU's bits (x86-64-v3 as clang's -mavx2 -mfma -mbmi -mbmi2 -mmovbe -mlzcnt -mpopcnt).
inline CpuBits ReadCpuBits() {
  CpuBits c;
  int r[4] = {};
  __cpuid(r, 0);
  if (r[0] < 7) return c;
  __cpuid(r, 1);
  const unsigned ecx1 = unsigned(r[2]);
  c.f16c = ecx1 & (1u << 29), c.fma = ecx1 & (1u << 12), c.movbe = ecx1 & (1u << 22), c.popcnt = ecx1 & (1u << 23), c.avx = ecx1 & (1u << 28);
  const bool osxsave = ecx1 & (1u << 27);
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = unsigned(r[1]);
  c.bmi1 = ebx7 & (1u << 3), c.avx2 = ebx7 & (1u << 5), c.bmi2 = ebx7 & (1u << 8);
  __cpuid(r, int(0x80000000u));
  if (unsigned(r[0]) < 0x80000001u) return c;
  __cpuid(r, int(0x80000001u));
  c.lzcnt = unsigned(r[2]) & (1u << 5);
  // The YMM registers must also be saved by Windows across thread switches (bit 2 of the enabled state: AVX).
  c.os_ymm = osxsave && (GetEnabledXStateFeatures() & (1ull << 2));
  return c;
}

inline bool CpuRunsV3() { return V3Capable(ReadCpuBits()); }

// The product version string of an exe (its VERSIONINFO, written by CMakeLists.txt: WWE2K14_VERSION + commit); empty = none.
inline std::string ExeProductVersion(const std::filesystem::path& exe) {
  DWORD h = 0;
  const DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &h);
  if (!n) return {};
  std::vector<char> buf(n);
  if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return {};
  void* v = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904B0\\ProductVersion", &v, &len) || !v || !len) return {};
  const wchar_t* w = static_cast<const wchar_t*>(v);
  std::string out;
  for (UINT i = 0; i < len && w[i]; ++i) out += char(w[i] < 128 ? w[i] : '?');
  return out;
}

// Both exes carry a version and it is the same one. An exe without VERSIONINFO (an older package) never matches: the AVX2 exe is then not started.
inline bool ExeVersionsMatch(const std::filesystem::path& base, const std::filesystem::path& v3) {
  const std::string a = ExeProductVersion(base), b = ExeProductVersion(v3);
  return !a.empty() && a == b;
}

// The exe a Play starts: Settings "Game exe" (game_exe), else the automatic pick; `flags` = the Advanced extra flags.
inline ExePick PickGameExe(const std::filesystem::path& launcher_dir, std::string_view game_exe = {}, std::string_view flags = {}) {
  std::error_code ec;
  const std::filesystem::path v3 = launcher_dir / "wwe2k14_v3.exe";
  const bool present = std::filesystem::is_regular_file(v3, ec);
  const bool match = present && ExeVersionsMatch(launcher_dir / "wwe2k14.exe", v3);
  const std::wstring wd = launcher_dir.wstring();  // UTF-8 for the pure function (the launcher's Widen() reads it back)
  std::string dir(size_t(WideCharToMultiByte(CP_UTF8, 0, wd.c_str(), int(wd.size()), nullptr, 0, nullptr, nullptr)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wd.c_str(), int(wd.size()), dir.data(), int(dir.size()), nullptr, nullptr);
  return GameExeFor(game_exe, dir, present, match, flags, ReadCpuBits());
}

}  // namespace launcher
