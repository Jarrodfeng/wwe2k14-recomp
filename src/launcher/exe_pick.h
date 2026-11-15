// wwe2k14 launcher - the pure part of "which game exe": CPU feature bits in, exe name out. No Windows, no cpuid, no files (Session 102 lane W1).
// cpu_features.h reads the real CPU and the real folder and calls this; tools\launcher\s102_tests\exe_pick_test.cpp calls it with made-up CPUs.
//
// Two builds ship side by side: wwe2k14.exe ("base": SSE4.1, runs on everything the project supports) and wwe2k14_v3.exe ("avx2": the same
// game built for x86-64-v3). A CPU without every v3 feature dies on its first VEX instruction with no message, so NOTHING overrides the CPU
// check: --wwe2k14_build=avx2 on a CPU that cannot run it still gives the base build.
#pragma once

#include <string>
#include <string_view>

namespace launcher {

// Every bit the v3 code may use (clang -mavx2 -mfma -mbmi -mbmi2 -mmovbe -mlzcnt -mpopcnt), plus Windows saving the YMM registers.
struct CpuBits {
  bool fma = false, movbe = false, popcnt = false, avx = false, os_ymm = false, bmi1 = false, avx2 = false, bmi2 = false, lzcnt = false, f16c = false;
};

inline bool V3Capable(const CpuBits& c) {
  return c.fma && c.movbe && c.popcnt && c.avx && c.os_ymm && c.bmi1 && c.avx2 && c.bmi2 && c.lzcnt && c.f16c;
}

enum class BuildChoice { kAuto, kAvx2, kBase };

// The value of --wwe2k14_build=<avx2|base|auto> in a flag string (the last one wins); anything else, or none, is kAuto.
inline BuildChoice ParseBuildChoice(std::string_view flags) {
  BuildChoice r = BuildChoice::kAuto;
  constexpr std::string_view key = "--wwe2k14_build=";
  for (size_t at = flags.find(key); at != std::string_view::npos; at = flags.find(key, at + 1)) {
    if (at != 0 && flags[at - 1] != ' ' && flags[at - 1] != '\t') continue;
    size_t b = at + key.size(), e = b;
    while (e < flags.size() && flags[e] != ' ' && flags[e] != '\t') ++e;
    const std::string_view v = flags.substr(b, e - b);
    r = v == "avx2" ? BuildChoice::kAvx2 : v == "base" ? BuildChoice::kBase : BuildChoice::kAuto;
  }
  return r;
}

// The flag string without the --wwe2k14_build= tokens (the game does not know them).
inline std::string StripBuildChoice(const std::string& flags) {
  constexpr std::string_view key = "--wwe2k14_build=";
  std::string out;
  size_t i = 0;
  while (i < flags.size()) {
    const bool at_word = i == 0 || flags[i - 1] == ' ' || flags[i - 1] == '\t';
    if (at_word && flags.compare(i, key.size(), key) == 0) {
      while (i < flags.size() && flags[i] != ' ' && flags[i] != '\t') ++i;
      while (i < flags.size() && (flags[i] == ' ' || flags[i] == '\t')) ++i;
      continue;
    }
    out += flags[i++];
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  return out;
}

// true = start wwe2k14_v3.exe. The CPU check always applies; the v3 file must exist and be the same build as wwe2k14.exe (a v3 exe left over from
// another release must never be started beside a newer base exe); kBase always wins.
inline bool UseV3(BuildChoice choice, bool v3_file_present, bool versions_match, const CpuBits& cpu) {
  if (choice == BuildChoice::kBase) return false;
  return v3_file_present && versions_match && V3Capable(cpu);
}

struct ExePick {
  std::string path;  // the exe to start
  std::string note;  // non-empty = say it in the launcher's status line
};

namespace detail {
inline size_t LastSep(std::string_view p) { return p.find_last_of("\\/"); }
inline std::string Lower(std::string_view p) {
  std::string r(p);
  for (char& c : r) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return r;
}
}  // namespace detail

// The whole decision, pure: Settings "Game exe" (empty = automatic), the launcher folder (no trailing separator), the Advanced extra flags,
// whether wwe2k14_v3.exe is in the folder and has the same version as wwe2k14.exe, and the CPU. Called whenever the exe is needed (startup,
// Settings changes, the Extra flags field), so --wwe2k14_build takes effect without a reload.
inline ExePick GameExeFor(std::string_view game_exe_setting, std::string_view launcher_dir, bool v3_file_present, bool versions_match,
                          std::string_view extra_flags, const CpuBits& cpu) {
  ExePick r;
  if (!game_exe_setting.empty()) {
    r.path = std::string(game_exe_setting);
    const size_t sep = detail::LastSep(game_exe_setting);
    const std::string leaf = detail::Lower(sep == std::string_view::npos ? game_exe_setting : game_exe_setting.substr(sep + 1));
    if (leaf == "wwe2k14_v3.exe" && !V3Capable(cpu)) {  // an explicit AVX2 exe on a CPU that cannot run it would die on its first instruction
      r.path = std::string(sep == std::string_view::npos ? std::string_view() : game_exe_setting.substr(0, sep + 1)) + "wwe2k14.exe";
      r.note = "This CPU cannot run the AVX2 game exe named in Settings (Game exe): using wwe2k14.exe instead.";
    }
    return r;
  }
  r.path = std::string(launcher_dir) + "\\" + (UseV3(ParseBuildChoice(extra_flags), v3_file_present, versions_match, cpu) ? "wwe2k14_v3.exe" : "wwe2k14.exe");
  return r;
}

}  // namespace launcher
