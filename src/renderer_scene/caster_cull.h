// The caster passes' face cull (Session 111, lane CASTCULL; docs\research\s111\CASTCULL.md). Header-only; the unit is tools\scene\castcull_unit.cpp.
// The game draws every caster with the cull on and leaves the face to the recorder's winding word (+116). The shadow module frame (sub_827E95D8 +88)
// turns the cull on around the props map and map B with the reset's winding 2 = device cull mode 6 (cull back, front clockwise). The plane mask pass
// (sub_827EEF80 +80 / +8C) turns it on with winding 1 = mode 2 (cull back, front counter-clockwise) around the whole mask walk, the rope proxies included
// (the ring task sub_825CF2F8 +2F4..+318 puts them in the module's proxy list, which sub_827EE190 draws inside the walk).
// Ours lands every caster vertex at the game's clip position (the pass's mirror row undoes the loader's Y mirror; the rope proxy is in the game's space with
// no mirror) and system[1] = -1 puts the image the game's way up, so the device mode's bits map straight onto ours (as the stream reader decodes the
// game's own draws: renderer_adapter\reader.h R3-R5). An open caster seen from its culled side stops casting; a closed body casts the same picture.
#pragma once
#include <cstdint>

namespace rdr_scene {

struct CasterFace {
  uint8_t cull;         // VkCullModeFlags: 0 none, 1 front, 2 back, 3 both
  uint8_t front_face;   // VkFrontFace: 0 counter-clockwise, 1 clockwise
};

// The game's device cull mode (bit 0 cull front, bit 1 cull back, bit 2 front = clockwise on screen) -> ours.
constexpr CasterFace CasterFaceOfDevice(uint32_t mode) {
  return CasterFace{uint8_t(((mode & 1u) ? 1u : 0u) | ((mode & 2u) ? 2u : 0u)), uint8_t((mode & 4u) ? 1u : 0u)};
}

// The passes' DrawState values (plain constants: q rstdiff reads them by name).
inline constexpr uint8_t kCasterCull = 2;          // every caster pass culls back faces
inline constexpr uint8_t kCasterFrontShadow = 1;   // the props map's and map B's casters (mode 6): front clockwise
inline constexpr uint8_t kCasterFrontPlane = 0;    // the plane mask's casters and the rope proxies (mode 2): front counter-clockwise
static_assert(CasterFaceOfDevice(6).cull == kCasterCull && CasterFaceOfDevice(6).front_face == kCasterFrontShadow, "the shadow frame's mode 6");
static_assert(CasterFaceOfDevice(2).cull == kCasterCull && CasterFaceOfDevice(2).front_face == kCasterFrontPlane, "the plane mask pass's mode 2");

// Vulkan's face test on framebuffer positions (x right, y down): a = -1/2 sum(x_i y_i+1 - x_i+1 y_i), a > 0 = counter-clockwise. True = the triangle is
// dropped. A zero-area triangle covers nothing either way (reported as kept). The sum is taken about p0 (the same value: the area does not move with a
// translation), so a thin triangle far from the origin keeps its sign in float.
inline bool CasterCulled(CasterFace f, const float p0[2], const float p1[2], const float p2[2]) {
  const float ux = p1[0] - p0[0], uy = p1[1] - p0[1], vx = p2[0] - p0[0], vy = p2[1] - p0[1];
  const float a = -0.5f * (ux * vy - vx * uy);
  if (a == 0.f) return false;
  const bool front = f.front_face ? a < 0.f : a > 0.f;
  return (f.cull & (front ? 1u : 2u)) != 0;
}

}  // namespace rdr_scene
