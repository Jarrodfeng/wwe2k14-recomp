// S103 lane SLV (rewritten in SLV2 after the Opus review): the blank plane rule for the stream-free build. Pure: no GPU, no game, no SDK include (tools\scene\slv_unit.cpp tests it with planted twins).
//
// THE RULE: the plane shadow chain makes nothing a reader can use when the ground square the blur draws (the game's fixed square, +-640 world units at the plane height) has EVERY corner at clip w <= 0
// (fit x camera, row 3): the sentinel fit of sub_827ED960 (no corner projects). This is the `behind == 4` case live_map_plane.cpp already ends in: the chain stops after the mask and the map stays RAW, which
// no reader accepts. The rule moves that test ABOVE the mask pass, so the mask (the one pass that still ran) is saved; the picture is the same (a raw map and no map both give the plain mat).
//
// What the rule is NOT: weight 0 (module+104, fade[9]). The first version skipped on weight 0 alone. That was wrong: the weight drives no shader (the yBlur plane constants are c33, c36-39 and
// c40; c35 is uploaded and never read; the mask's c6.w is unused; yProp_PSMLM has no weight). sub_827ED960 +784 gives weight = ((1 - |f22|) + (1 - |f23|)) * 0.5, which is 0 in two cases:
//   (a) no corner projects: the sentinel fit (y scale -1): this rule's case;
//   (b) the ground square fills the screen height: a REAL shadow the game draws. Weight 0 alone must never skip it.
#pragma once
#include <atomic>
#include <cmath>

#include "scene_record.h"

namespace rdr_scene {

// The switch: wwe2k14_rdr_skip_blank_plane (default false, start_settings.cpp stores it once in the stream-free build; rigs: frame_rig WWE_RDR_SKIP_BLANK_PLANE=1 turns it on, =0 off).
inline std::atomic<bool> g_skip_blank_plane{false}; // BOTH-BUILDS: the stream build stores it per frame from its switch table, the stream-free build once at start (start_settings.cpp)

// The game's fixed ground square (PlaneScale.x / .z of the blur draws, two captures; live_map_plane.cpp kBlurHalfExtent).
inline constexpr float kPlaneSquareHalf = 640.0f;

// How many of the four ground-square corners are at or behind the camera (clip w <= 0, or not a number) under M = fit x camera (the readers' projection, row-major as the record holds it) at plane height y0.
inline int PlaneSquareCornersBehind(const float m4[16], float y0, float half = kPlaneSquareHalf) {
  const float cx[4] = {-half, half, half, -half}, cz[4] = {-half, -half, half, half};
  int behind = 0;
  for (int k = 0; k < 4; ++k) behind += !(m4[12] * cx[k] + m4[13] * y0 + m4[14] * cz[k] + m4[15] > 0.f);
  return behind;
}

// True when the chain is blank: no corner of the ground square projects.
inline bool PlaneChainBlank(const float m4[16], float y0) { return PlaneSquareCornersBehind(m4, y0) == 4; }

}  // namespace rdr_scene
