// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


// S101 lane 109 (LS0; docs\renderer\PER_DRAW_LIGHTS.md): the per-draw light state. Every per-draw light constant of a model draw is a verbatim copy of
// one record per thread slot, the "render camera record" R (2208 bytes, the game's global 0x8392DDA4 -> +4 array, index = the thread slot). A LightState
// is what the packer / binder read of R, decoded to host floats, in R's own order. Two-level table so that ~300 model calls a frame share <= 8 states.
inline constexpr uint32_t kLsLightFloats = 224;     // R+0..895 (the install's block, from the light object +48)
// Float offsets inside LightState::light (R byte offset / 4): the named parts of the install block (PER_DRAW_LIGHTS.md 1.1).
inline constexpr uint32_t kLsDir = 4;       // R+16: 5 light directions, 4 floats each (world space; w as stored)
inline constexpr uint32_t kLsCol = 24;      // R+96: 5 light colours rgbw (w = the lobe weight), 4 floats each; light 4 = the KeyLight
inline constexpr uint32_t kLsBlockA = 88;   // R+352 = light object +400: the sky colour, SkyCol [C sub_82BE06B8 / sub_82BDB460, SKYGROUND.md]
inline constexpr uint32_t kLsBlockB = 92;   // R+368 = light object +416: the ground colour, GroundCol [C]
struct LightState {
  float light[kLsLightFloats];   // R+0..895 as floats (word-swapped from the guest)
  float shadow_m[3][16];         // R+912, R+976, R+1040: three 4x4 matrices of the shadow fields
  float shadow_f[16];            // R+1104..1163 (15 floats; [15] = 0): the ShadowPrm source floats
  float shadow_x[2][8];          // R+2144, R+2176: two 32-byte blocks (the packer copies them to the shadow sub-block)
  float map_wh[2];               // the shadow map size, G+208 / G+212 (floats of the word at 0x8392DDA4); 0 = not read
  uint32_t slot;                 // the thread slot the state was read from
  uint32_t flags;                // LightStateFlags
};
enum LightStateFlags : uint32_t { kLsMapKnown = 1, kLsSynthetic = 2 };   // kLsSynthetic (S103 A8): written from the latch by wrestler_light_latch.h, not a walked R: its shadow fields are zero; d3::RecordIndex::Add skips its refs
static_assert(sizeof(LightState) == 1232, "LightState is a flat POD table");
// One model call (sub_82C04DF0): the engine object (DrawOrigin::object), the 0-based ordinal of this object's model calls in the frame, the state it read.
struct LightRef {
  uint32_t object = 0;
  uint32_t ordinal = 0;
  uint32_t state = 0;            // index into kScLightStates
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the call ran: the join to the stream's frame is by this, not the record header
};
static_assert(sizeof(LightRef) == 16, "LightRef is a flat POD table");

}  // namespace rdr_scene
