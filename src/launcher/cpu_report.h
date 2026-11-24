// wwe2k14 PC launcher - the CPU lines of the problem report's about.txt (S108 lane MOB): the vendor and the instruction-set features the game's two builds
// care about (exe_pick.h CpuBits), and whether the faster AVX2 build can run on this CPU. Pure text: launcher_main.cpp PcDetailsText reads CPUID and calls
// this; tools\report\redact_unit.cpp calls it with made-up CPUs. The brand string, cores and threads are already written by HardwareText / PcDetailsText.
#pragma once

#include <string>

#include "exe_pick.h"

namespace launcher {

inline std::string CpuFeaturesText(const std::string& vendor, bool sse41, const CpuBits& c) {
  auto yn = [](bool b) { return b ? "yes" : "no"; };
  std::string t = "CPU vendor: " + (vendor.empty() ? std::string("unknown") : vendor) + "\n";
  t += std::string("CPU features: SSE4.1 ") + yn(sse41) + ", AVX " + yn(c.avx) + ", AVX2 " + yn(c.avx2) + ", FMA " + yn(c.fma) + ", BMI1 " + yn(c.bmi1) +
       ", BMI2 " + yn(c.bmi2) + ", F16C " + yn(c.f16c) + ", MOVBE " + yn(c.movbe) + ", LZCNT " + yn(c.lzcnt) + ", POPCNT " + yn(c.popcnt) +
       ", Windows saves YMM " + yn(c.os_ymm) + "\n";
  std::string missing;
  auto need = [&](bool have, const char* name) {
    if (!have) missing += (missing.empty() ? "" : ", ") + std::string(name);
  };
  need(c.avx, "AVX"), need(c.avx2, "AVX2"), need(c.fma, "FMA"), need(c.bmi1, "BMI1"), need(c.bmi2, "BMI2"), need(c.f16c, "F16C"), need(c.movbe, "MOVBE"),
      need(c.lzcnt, "LZCNT"), need(c.popcnt, "POPCNT"), need(c.os_ymm, "Windows saving YMM");
  t += V3Capable(c) ? "AVX2 game build (wwe2k14_v3.exe): this CPU can run it\n"
                    : "AVX2 game build (wwe2k14_v3.exe): this CPU cannot run it (missing: " + missing + "); the base build is used\n";
  return t;
}

}  // namespace launcher
