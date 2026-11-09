// Session 98 (flash fix): the crowd's camera flashes of the entrance look, as a rule. Our own code, written from the game's draws.
//
// What the game does [C: run 6818 captures rp_ring / rp_ringin (events 12583-12608: VS 1D3A5C66 yFlashDefault, PS 9A39D20F = texture x
// vertex colour, blend SrcAlpha + One, drawn after the post combine into the finished picture) and the draw probes of runs 6816 (40
// frames) + 6818 (8 frames), 84 flash draws]:
//  - the sprite is the arena pac's entry 31000 (a JBOY in every arena pac checked: bg00-bg28 [C, tools\assets\find_bytes.py on its
//    vertex bytes]): a soft square, a turned square at half alpha and two long thin diagonal rays, with its own texture;
//  - one draw per flash, a world matrix c76..c78 = a camera-facing frame times a scale, the centre c214 = the flash's position;
//  - a flash lives two guest frames: scale 0.5 on the first, 0.15 on the second, then gone [C: frames 10547 -> 10548, two flashes];
//  - its colour comes from a four-entry table (c0 = c1 = c181 = c182; rgb as below, w 0.7) and steps to the next entry on its second
//    frame [C: the same two flashes: entry 2 -> 3, 1 -> 2];
//  - the vertex shader lights it with two lights along +-x carrying the colour plus the colour as ambient: lit = colour x (1 + |n.x|),
//    n = the flash's facing axis; alpha = c185.x (0.5) x the vertex alpha; the whole flash is hidden when the depth at its centre is
//    nearer than the flash (vs_flash.vert: the vertex texture lookup);
//  - positions: EVERY one of the 84 lies on one of two 45-degree cones about the ring's centre: height = r - 2660 for r in 3104..4116
//    (79 of them) and height = r - 1660 for r in 1927..1981 (5) [C, to 0.001]; the outer cone's angles (atan2(z, x)) all fall in
//    -180..45 degrees (79 of 79), the inner one's anywhere;
//  - flashes only in the entrances (0 in the 20 match frames of run 6816), on average 1.56 new ones per guest frame.
//
// [U] still specific / not found (Session 98): where the cones, the arc, the colour table and the rate come from (not in the arena,
// event or prop pacs as floats or text, not in prtcls.pac (no flash / crowd names, no 2660), not in the executable's data as floats,
// no code immediate 2660 / 6.0 near a flash user; colour entry 1 really is (0.8, 6.0, 0.6)); measured on
// ONE arena (5) and two entrants (Cena, Orton), so other arenas use the same numbers until measured. The game's rate varies along the
// entrance (0 new flashes in both runs at Orton's stage / ramp frames, up to 6 in the ring); its rule is not found, so we use the mean
// as a steady rate. Our flashes are seeded by the scene clock (the entrance frame), so a render repeats exactly.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace assets::crowd_flash {

struct Flash {
  float pos[3] = {0, 0, 0};  // viewer space (y up)
  float scale = 0.5f;        // the sprite's scale (the game's world matrix)
  float rgb[3] = {1, 1, 1};  // the table colour (before the 1 + |n.x| lighting)
  float alpha = 0.5f;        // c185.x
};

struct Params {
  double births_per_frame = 1.56;  // mean new flashes per guest frame (runs 6816 + 6818, 27 entrance frames: 42 births)
  float far_r0 = 2660.f, far_up_min = 444.f, far_up_max = 1456.f;  // outer cone: height = r - far_r0
  float far_arc_lo = -180.f, far_arc_hi = 45.f;                    // its angles, degrees, atan2(z, x)
  float near_frac = 5.f / 84.f;                                    // share on the inner cone
  float near_r0 = 1660.f, near_up_min = 267.f, near_up_max = 321.f;
  float scale0 = 0.5f, scale1 = 0.15f;  // first / second frame
  float alpha = 0.5f;
  float colours[4][3] = {{0.6f, 0.8f, 0.6f}, {0.8f, 6.0f, 0.6f}, {0.6f, 0.6f, 0.8f}, {0.9f, 0.9f, 1.0f}};
};

inline uint32_t Mix(uint32_t x) {
  x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
  return x;
}
inline float Uniform(uint32_t& s) {
  s = Mix(s + 0x9E3779B9u);
  return float(s >> 8) * (1.f / 16777216.f);
}

// The flashes born at `frame`, as they look `phase` frames later (0 or 1).
inline void Births(int64_t frame, uint32_t seed, const Params& p, int phase, std::vector<Flash>& out) {
  uint32_t s = Mix(uint32_t(frame) * 0x85EBCA6Bu ^ Mix(uint32_t(uint64_t(frame) >> 32) + seed));
  const double limit = std::exp(-p.births_per_frame);
  int n = 0;
  for (double prod = Uniform(s); prod > limit && n < 32; prod *= Uniform(s)) ++n;  // Poisson count (inversion)
  const float kPi = 3.14159265f;
  for (int i = 0; i < n; ++i) {
    const bool inner = Uniform(s) < p.near_frac;
    const float t = Uniform(s), a = Uniform(s), c = Uniform(s);
    const float up = inner ? p.near_up_min + (p.near_up_max - p.near_up_min) * t : p.far_up_min + (p.far_up_max - p.far_up_min) * t;
    const float r = up + (inner ? p.near_r0 : p.far_r0);
    const float deg = inner ? -180.f + 360.f * a : p.far_arc_lo + (p.far_arc_hi - p.far_arc_lo) * a;
    int ci = int(c * 4.f) & 3;
    if (phase == 1) ci = (ci + 1) & 3;
    Flash f;
    f.pos[0] = r * std::cos(deg * kPi / 180.f), f.pos[1] = up, f.pos[2] = r * std::sin(deg * kPi / 180.f);
    f.scale = phase == 0 ? p.scale0 : p.scale1;
    for (int k = 0; k < 3; ++k) f.rgb[k] = p.colours[ci][k];
    f.alpha = p.alpha;
    out.push_back(f);
  }
}

// Every flash visible at guest frame `frame` (born this frame or the one before).
inline std::vector<Flash> At(int64_t frame, uint32_t seed = 0, const Params& p = Params()) {
  std::vector<Flash> v;
  Births(frame, seed, p, 0, v);
  Births(frame - 1, seed, p, 1, v);
  return v;
}

}  // namespace assets::crowd_flash
