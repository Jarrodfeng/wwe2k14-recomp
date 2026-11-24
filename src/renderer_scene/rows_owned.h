// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {

// S103 lane S (FRAME_CONTRACT.md 3): kScOwnedPasses rows, the same 16 bytes as rdr_own::OwnedPassStamp (node_switch.h; this header does not include it).
struct OwnedStamp {
  uint8_t pass = 0, decision = 0, why_not = 0, pad = 0;   // rdr_own::Pass, Decision (1 = Ours), WhyNot
  uint32_t inputs_mask = 0;
  uint64_t guest_frame = 0;
};
static_assert(sizeof(OwnedStamp) == 16, "OwnedStamp is rdr_own::OwnedPassStamp's layout");

}  // namespace rdr_scene
