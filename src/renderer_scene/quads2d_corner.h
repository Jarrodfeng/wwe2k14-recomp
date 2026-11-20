// Quads2DCorner (Session 105, lane A8; docs\research\s105\A8.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// A T_RectColor corner's colour. The game stores each corner as four ints 0..255 and the leaf (sub_82BC3F30) writes float(int) * float(1/255) into its vertices (listing 0x82BC4404 region: extsw, fcfid, frsp, fmuls by
// f31 = [0x820034FC]). A Quad2D vertex has two spare floats (verts[v][3..4]), not four, so the row keeps the INTEGERS: verts[v][3] = r | g << 8 | b << 16 (a whole number below 2^24: exact in a float), verts[v][4] = a.
// UnpackCorner makes the leaf's own floats back with the same multiply. No game or SDK includes.
#pragma once
#include <cmath>
#include <cstdint>

namespace rdr_scene::q2d {

inline constexpr uint32_t kSiteRectColorPrimWord = 13;   // the op-19 primitive word sub_82BC3F30 passes (a quad list)
inline constexpr float kInv255 = 0.003921569f;   // [0x820034FC] (the leaf's 1/255 as a float)

// rgba = the four floats the leaf wrote for one corner; false (nothing written) when one is not a whole byte / 255.
inline bool PackCorner(const float rgba[4], float& rgb_packed, float& a) {
  uint32_t n[4];
  for (int i = 0; i < 4; ++i) {
    const float f = rgba[i];
    if (!std::isfinite(f) || f < 0.0f || f > 255.0f * kInv255 * 1.0001f) return false;
    const float r = std::round(f / kInv255);
    if (std::fabs(r * kInv255 - f) > 1e-6f) return false;   // not an integer / 255 (the leaf's colours always are)
    n[i] = uint32_t(r);
  }
  rgb_packed = float(n[0] | (n[1] << 8) | (n[2] << 16)), a = float(n[3]);
  return true;
}
inline void UnpackCorner(float rgb_packed, float a, float out[4]) {
  const uint32_t p = uint32_t(rgb_packed);
  out[0] = float(p & 255u) * kInv255, out[1] = float((p >> 8) & 255u) * kInv255, out[2] = float((p >> 16) & 255u) * kInv255, out[3] = float(uint32_t(a)) * kInv255;
}
// A packed corner is valid when both numbers are whole and in range (the reader's and the drawer's check).
inline bool CornerValid(float rgb_packed, float a) {
  return rgb_packed >= 0.0f && rgb_packed <= 16777215.0f && rgb_packed == std::floor(rgb_packed) && a >= 0.0f && a <= 255.0f && a == std::floor(a);
}

}  // namespace rdr_scene::q2d
