#pragma once
// wwe2k14 - Session 94, idea 12: smaller up-front sizes for a phone with less than 6 GB of RAM (report_005: a 3.5 GB Mali-G57
// phone had 1.3 GB free before the first frame). One switch, --wwe2k14_low_ram=auto|on|off (default auto = a phone, i.e. an
// Android build, with total RAM under 6144 MB; "on" forces it for a test on a big phone or the PC). Each size is set only if its
// switch is still at default (a command-line value wins). Called from OnPreSetup BEFORE d3d_owned::Apply(), so the frame budget
// set here is not overwritten by the stack's 1024 (the SDK default without d3d_owned is 512).
#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(wwe2k14_low_ram, "auto", "wwe2k14",
                      "Session 94: smaller up-front sizes on a phone with under 6 GB of RAM: owned frame budget 384 MB, "
                      "texture cache 192/384 MB, keep-warm buffer 8 MB, upload pool kept to 32 MB, readbacks 64. auto = Android and total RAM < 6144 MB; on = force; off = never");

namespace wwe2k14::low_ram {

// Total physical RAM in MB (0 = unknown).
inline uint64_t TotalRamMb() {
#if defined(_WIN32)
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  return GlobalMemoryStatusEx(&m) ? uint64_t(m.ullTotalPhys >> 20) : 0;
#else
  FILE* f = std::fopen("/proc/meminfo", "r");
  if (!f) return 0;
  char line[128];
  uint64_t kb = 0;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "MemTotal:", 9) == 0) {
      kb = std::strtoull(line + 9, nullptr, 10);
      break;
    }
  }
  std::fclose(f);
  return kb >> 10;
#endif
}

inline void Apply() {
  const std::string mode = REXCVAR_GET(wwe2k14_low_ram);
  if (mode == "off") return;
  const uint64_t ram_mb = TotalRamMb();
  bool phone = false;
#if defined(__ANDROID__)
  phone = true;
#endif
  const bool forced = mode == "on";
  if (!forced && !(phone && ram_mb && ram_mb < 6144)) return;
  // Session 94 idea 12 (design out\s94\d11_d12_design.md 12.2). Each only if left at default.
  static constexpr std::pair<const char*, const char*> kSizes[] = {
      {"gpu_native_owned_frame_budget_mb", "384"},   // was 1024 with d3d_owned, 512 without (the owned-only latch drops a pass over the budget: counted)
      {"texture_cache_memory_limit_soft", "192"},    // was 384
      {"texture_cache_memory_limit_hard", "384"},    // was 768
      {"gpu_keep_warm_buffer_mb", "8"},              // was 32
      {"vulkan_upload_pool_keep_mb", "32"},          // was 0 (keep every page)
      {"vulkan_readback_max_buffers", "64"},         // was 256
  };
  for (const auto& [name, value] : kSizes) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  }
  // Session 94 M2a: the 384 MB owned frame budget is sized for the phone's 720p (scale 1); a larger resolution scale needs
  // scale^2 of it (a forced 1440p PC run dropped passes at 384 MB: 'pool' drops, broken pictures). The settings file has set
  // resolution_scale before this runs (OnPreSetup order).
  {
    const double scale = std::atof(rex::cvar::GetFlagByName("resolution_scale").c_str());
    if (scale > 1.0 && rex::cvar::GetFlagByName("gpu_native_owned_frame_budget_mb") == "384") {
      rex::cvar::SetFlagByName("gpu_native_owned_frame_budget_mb", std::to_string(int(384.0 * scale * scale + 0.5)));
    }
  }
  REXLOG_INFO("[quirks] low_ram: {} RAM {} MB -> frame budget {}, texture cache {}/{}, keep-warm {} MB, upload keep {}, "
              "readbacks {}",
              forced ? "forced" : "auto", ram_mb, rex::cvar::GetFlagByName("gpu_native_owned_frame_budget_mb"),
              rex::cvar::GetFlagByName("texture_cache_memory_limit_soft"),
              rex::cvar::GetFlagByName("texture_cache_memory_limit_hard"),
              rex::cvar::GetFlagByName("gpu_keep_warm_buffer_mb"), rex::cvar::GetFlagByName("vulkan_upload_pool_keep_mb"),
              rex::cvar::GetFlagByName("vulkan_readback_max_buffers"));
}

}  // namespace wwe2k14::low_ram
