// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {

inline constexpr uint32_t kScPartClocks = 50;
inline constexpr uint32_t kPartClockLayout = 1;
struct PartClock {
  uint32_t version = kPartClockLayout;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits)
  uint32_t object = 0;           // the stage model object (the join key of DrawOrigin::object / kScArenaWorlds)
  uint32_t part = 0;             // the first part this row applies to (rows of one object ascend; it covers the parts up to the next row)
  float time = 0.f;              // c186 for those parts: the part's time word
  float period = 0.f;            // the wrap period (+16): the update subtracts it when time + step exceeds it (sub_82BD5A98 0x82BD5AF4-0x82BD5B04)
  float step = 0.f;              // [model+248]: what one update adds (census; the consumer needs only `time`)
  uint32_t slot = 0xFFFFFFFFu;   // the stage array index of the model (the "arena:slot:K" number, [L] the same index); 0xFFFFFFFF = not from a stage array
};
static_assert(sizeof(PartClock) == 32, "PartClock is a flat POD table");

}  // namespace rdr_scene
