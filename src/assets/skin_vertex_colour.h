// S111 VERTEXSEAM (docs\research\s111\VERTEXSEAM.md): the colour bytes of a wrestler SKIN vertex (yCh_skin / yCh_skin_flex meshes).
// The game copies the file's S0 colour word unchanged into the GPU vertex (sub_82BD4BE0 +250..+264, flag bit 0x40) and the skin vertex
// shaders fetch it as four bytes / 255 in the order (word byte 1, 2, 3, 0) = file bytes (25, 26, 27, 24) of the vertex's S0 record (the
// harness's model of an 8_8_8_8 fetch, src\scene\xenos_vs_math.glsl AvFetchColorAt). Our skin port reads that element as inExtra
// (front_end.cpp LayoutFor -> Vertex::color): x = byte 25, the first specular lobe's gain (fx_skin.glsl:300); w = byte 24, the packed
// ID-map slot (low 3 bits) and wrinkle slot (the rest) (fx_skin.glsl:140-153; stock faces carry 0 / 8 / 24 / 40 = wrinkle slots 0 / 1 / 3 / 5).
// Before this lane the loader forced w = 255: every face vertex read ID slot 7 and wrinkle slot 31 (-> the fallback slot 1), and the
// plain FX_WRINKLES variants drew every wrinkle at full depth (w = 1.0) where the game draws byte 24 / 255.
#pragma once

#include <cstdint>

#include "load_plants.h"   // S111 VERTEXSEAM: load_detail::VcolOldAlphaPlant (the planted twin; the rig / unit sets it, the game exe never does)
#include "vertex_decode.h"   // S111 GUESTVTX: vdecode::GameColour, the one colour rule (the created-superstar decoder calls it too)

namespace assets {

// p = the vertex's four colour bytes in the file (S0 record + 24). out = Vertex::color (R G B A as the effects read inExtra).
// old_rule = the planted twin (the old A = 255).
inline void SkinVertexColourRule(const uint8_t* p, bool old_rule, uint8_t out[4]) {
  // S111 GUESTVTX: the word as the game's fetch reads it (vertex_decode.h): R G B = file bytes 25, 26, 27 (one grey in every file measured:
  // S98 / WETLOOK histograms), A = byte 24.
  const uint32_t c = vdecode::GameColour(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
  for (int k = 0; k < 4; ++k) out[k] = uint8_t(c >> (8 * k));
  if (old_rule) out[3] = 0xFF;
}

inline void SkinVertexColour(const uint8_t* p, uint8_t out[4]) {
  SkinVertexColourRule(p, load_detail::VcolOldAlphaPlant(), out);
  if (load_detail::VdecOldPlant()) out[1] = out[2] = p[1];   // S111 GUESTVTX planted twin: the old rgb (byte 25 three times)
}

// The decode our skin port does with w (fx_skin.glsl:144-153), for the unit: packed = int(w * 255.5); ID slot = low 3 bits; wrinkle slot =
// the rest, past slot 5 the game falls back to slot 1.
inline int SkinPackedSlots(uint8_t alpha) { return int(float(alpha) / 255.0f * 255.5f); }
inline int SkinIdSlot(uint8_t alpha) { return SkinPackedSlots(alpha) & 7; }
inline int SkinWrinkleSlot(uint8_t alpha) {
  const int s = SkinPackedSlots(alpha) >> 3;
  return s > 5 ? 1 : s;
}

}  // namespace assets
