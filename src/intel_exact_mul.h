// Session 76 (an Intel HD Graphics 520 tester, v0.2.12, NATIVE at 720p: "Black eyes on Superstars only on
// entrances"). Not reproduced on the 4090. RenderDoc (run 4829, docs in SESSION76_SUMMARY.md): entrances draw the eyes
// with their own pixel shader variant (more pow maths: log2 / exp2), and the tester's driver reports
// shaderDenormFlushToZeroFloat32 (the 4090's does not), so their translated shaders flush tiny values to 0 and a log2 /
// rsq of one becomes infinite. NATIVE's multiply shortcuts (gpu_native_ps_mul_mode 2, gpu_native_vs_mul_mode 1,
// gpu_native_vs_mul_fetch_finite) drop the 360's rule (0 x anything = 0) where they assume the operands finite: 0 x inf
// is then NaN, drawn black. The likely cause, unconfirmed (Jarrod: ship the fix rather than a flag test). On an Intel
// GPU (the one the game uses, src\gpu_pick.h), NATIVE keeps the rule exact, whatever the command line says.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <string>
#include <utility>

#include "gpu_pick.h"

REXCVAR_DEFINE_INT32(wwe2k14_intel_exact_mul, 1, "wwe2k14",
                     "Session 76: 1 = on an Intel GPU, NATIVE keeps the 360's multiply rule exact "
                     "(gpu_native_ps_mul_mode 0, gpu_native_vs_mul_mode 0, gpu_native_vs_mul_fetch_finite false, "
                     "whatever the command line says; an Intel tester's black eyes in entrances); 0 = the flags as "
                     "given; 2 = on any GPU (tests)")
    .range(0, 2);

namespace wwe2k14::intel_exact_mul {

inline constexpr uint32_t kVendorIntel = 0x8086;

// OnPreSetup, after gpu_pick::Choose() (before any shader is translated).
inline void Configure() {
  const int mode = REXCVAR_GET(wwe2k14_intel_exact_mul);
  const bool intel = gpu_pick::g_known && gpu_pick::g_chosen.vendor == kVendorIntel;
  if (mode == 0 || (mode == 1 && !intel)) return;
  for (const auto& [name, value] : {std::pair{"gpu_native_ps_mul_mode", "0"}, std::pair{"gpu_native_vs_mul_mode", "0"},
                                    std::pair{"gpu_native_vs_mul_fetch_finite", "false"}}) {
    const std::string was = rex::cvar::GetFlagByName(name);
    if (was == value) continue;
    rex::cvar::SetFlagByName(name, value);
    REXLOG_INFO("[wwe2k14][gpu] exact multiply rule ({}): {} = {} (was {})",
                intel ? gpu_pick::g_chosen.name : std::string("--wwe2k14_intel_exact_mul=2"), name, value, was);
  }
}

}  // namespace wwe2k14::intel_exact_mul
