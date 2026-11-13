// wwe2k14 - Session 101, lane 176 (Jarrod: "the game's default behaviour culls the audience outside of its camera field (16:9 by default) so
// naturally ultrawide audience members have been getting reactively culled by the game itself"). The stands crowd's own screen test, fed the
// DISPLAYED aspect: members inside the wider view are kept. The rule (crowd_wide_core.h) and the evidence (docs\renderer\CROWD_WIDE.md).
//
// --wwe2k14_crowd_fill_wide = auto | off   (default auto)
//   auto  on exactly when the display is not 16:9: ultrawide.h turned --wwe2k14_aspect (auto / 21:9 / 32:9 / W:H, the launcher's ASPECT, the
//         phone launcher's screen) into the SDK's gpu_wide_factor k (Hor+) or gpu_tall_factor t (Vert+); the test then sees x / k, y / t.
//         On 16:9 both are 1 and every hook returns before touching anything: the game's own test, byte for byte.
//   off   the game's own 16:9 test whatever the display (the plant for the proof; a diagnosis switch)
// Three mid-function hooks (config\crowd_wide_hooks.toml), each on the line where the game has the test's matrix on its stack:
//   0x8250E1D0 in sub_8250E110 (the member test)       0x8250DC94 in sub_8250DC10 (the section test, via sub_8250E400)
//   0x8250E858 in sub_8250E7E0 (the seated floor people's test, sets / clears bit 31 of [record + 12], before the multiply)
// and the native copy of sub_8250E110 (nf\frustum_cull.h, off by default) applies the same scale, so verify mode stays equal.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "crowd_wide_core.h"

REXCVAR_DEFINE_STRING(wwe2k14_crowd_fill_wide, "auto", "wwe2k14",
                      "Session 101 lane 176: the stands crowd's screen test follows the displayed aspect. auto (default) = widened when the display "
                      "is wider / taller than 16:9 (gpu_wide_factor / gpu_tall_factor from --wwe2k14_aspect), the game's own 16:9 test otherwise; "
                      "off = always the game's own; k=<wide>,t=<tall> = explicit factors (what a recording carries). See src/crowd_wide.h");

namespace wwe2k14::crowd_wide {

inline std::atomic<uint32_t> g_x_bits{0x3F800000u}, g_y_bits{0x3F800000u};  // 1.0f, 1.0f
inline std::atomic<uint32_t> g_calls{0};
inline std::atomic<uint64_t> g_member_applied{0}, g_section_applied{0}, g_floor_applied{0}, g_stage_applied{0};  // hook hits that scaled (logged at the census point in Current())

inline float BitsToF(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
inline uint32_t FToBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// The cvars are read on the first call and every 1024th (they are fixed before the graphics system starts; a live change is followed).
inline Scale Current() {
  const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed);
  if ((n & 1023u) == 0) {
    const double wide = std::strtod(rex::cvar::GetFlagByName("gpu_wide_factor").c_str(), nullptr);
    const double tall = std::strtod(rex::cvar::GetFlagByName("gpu_tall_factor").c_str(), nullptr);
    const Scale s = FromSwitch(REXCVAR_GET(wwe2k14_crowd_fill_wide), wide, tall);
    const uint32_t xb = FToBits(s.x), yb = FToBits(s.y);
    if (xb != g_x_bits.load(std::memory_order_relaxed) || yb != g_y_bits.load(std::memory_order_relaxed) || n == 0) {
      g_x_bits.store(xb, std::memory_order_relaxed);
      g_y_bits.store(yb, std::memory_order_relaxed);
      REXLOG_INFO("[wwe2k14][crowd_wide] the stands crowd's screen test: x x {:.4f} (k {:.3f}), y x {:.4f} (t {:.3f}) ({})", s.x,
                  s.x != 1.0f ? 1.0 / s.x : 1.0, s.y, s.y != 1.0f ? 1.0 / s.y : 1.0, s.On() ? "widened to the display" : "the game's own 16:9 test");
    }
  }
  if ((n & 1023u) == 0) {  // census point: the counters prove both hooks fired (the section hook runs once per update, the member hook 20 x per update)
    static std::atomic<uint32_t> logged{0};
    const uint32_t k = logged.fetch_add(1, std::memory_order_relaxed);
    if (g_member_applied.load() + g_section_applied.load() + g_floor_applied.load() + g_stage_applied.load() > 0 && (k < 6 || k % 20 == 0))
      REXLOG_INFO("[wwe2k14][crowd_wide] census: member hook applied {}, section hook applied {}, floor hook applied {}, stage frustum hook applied {}", g_member_applied.load(),
                  g_section_applied.load(), g_floor_applied.load(), g_stage_applied.load());
  }
  return Scale{BitsToF(g_x_bits.load(std::memory_order_relaxed)), BitsToF(g_y_bits.load(std::memory_order_relaxed))};
}

inline uint8_t* GuestPtr(uint32_t address) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address); }

}  // namespace wwe2k14::crowd_wide

// sub_8250E110 at 0x8250E1D0 (the line after `bl 0x822505b8` returned): r1 = the frame; the camera's matrices multiplied in place at r1 + 112
// (four rows of 16 bytes, a row vector times it), f29 = m00 K and f28 = m11 K (the member radius scales, loaded from the unmultiplied matrix at
// 0x8250E1B8-0x8250E1C8). Scales output x by 1/k and y by 1/t in both. Listing: docs\renderer\CROWD_WIDE.md section 2.
void wwe2k14_crowd_wide_member(PPCRegister& r1, PPCRegister& f28, PPCRegister& f29) {
  using namespace wwe2k14::crowd_wide;
  const Scale s = Current();
  if (!s.On()) return;
  float r28 = float(f28.f64), r29 = float(f29.f64);
  ApplyMember(GuestPtr(r1.u32), s, r28, r29);
  f28.f64 = double(r28);
  f29.f64 = double(r29);
  g_member_applied.fetch_add(1, std::memory_order_relaxed);
}

// sub_8250DC10 at 0x8250DC94 (the line after `bl 0x82296630` returned): r1 + 160 = the product (rows of 16 bytes), r1 + 96 = the unmultiplied
// camera matrix whose m00 the radius uses (lfs f24,96(r1) at 0x8250DCA4, after this point). The section test is x only.
void wwe2k14_crowd_wide_section(PPCRegister& r1) {
  using namespace wwe2k14::crowd_wide;
  const Scale s = Current();
  if (s.x == 1.0f) return;
  ApplySection(GuestPtr(r1.u32), s);
  g_section_applied.fetch_add(1, std::memory_order_relaxed);
}

// sub_8250E7E0 at 0x8250E858 (the seated floor people's screen test, run every draw): r1 + 96 = the copy of camera +176, not yet multiplied by the
// view. Its member box is +-0.9 (not 1); the radius ([[member + 152] + 20] + the game's term) times m00 K follows the scaled copy.
void wwe2k14_crowd_wide_floor(PPCRegister& r1) {
  using namespace wwe2k14::crowd_wide;
  const Scale s = Current();
  if (!s.On()) return;
  ApplyFloor(GuestPtr(r1.u32), s);
  g_floor_applied.fetch_add(1, std::memory_order_relaxed);
}

// Lane 234: sub_82769B98 (the stage visibility frustum, called every update from sub_826989D8 on the object at 0x837689DC + 16) at 0x82769D10: r31 = the frustum
// (8 corners of 16 bytes at its start, camera space, before the loop that carries them to world space and builds the planes). Corner x x k, y x t.
void wwe2k14_crowd_wide_stage(PPCRegister& r31) {
  using namespace wwe2k14::crowd_wide;
  const Scale s = Current();
  if (!s.On()) return;
  ApplyStageFrustum(GuestPtr(r31.u32), s);
  g_stage_applied.fetch_add(1, std::memory_order_relaxed);
}
