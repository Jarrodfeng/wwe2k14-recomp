// The skinned prop vertex, as the game's converted vertex holds it (Session 101, lane 49; shared by the arena's placed props, slots 390-397, and the ring pieces,
// docs\renderer\RING_PROMOTION_MAP.md 1.3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes.
//
// What the stream holds for the `*_skinned_*_vertex` variants (stride 40, the fetch order of the vertex program) [C: Check B 7051, every vertex of the 5,456 arena prop draws,
// tools\ring\prop_vertex_check.cpp]:
//   @0  f32 x4  position (w = 1)                       VK_FORMAT_R32G32B32A32_SFLOAT  (109)
//   @16 u8  x4  weight word, UNORM                     VK_FORMAT_R8G8B8A8_UNORM       (37)
//   @20 u8  x4  bone ids, UINT                         VK_FORMAT_R8G8B8A8_UINT        (41)
//   @24 s16 x4  normal (w = 0)                         VK_FORMAT_R16G16B16A16_SNORM   (92)
//   @32 u8  x4  vertex colour                          VK_FORMAT_R8G8B8A8_UNORM       (37)
//   @36 f16 x2  uv (u = |file u|: the recompiled packer drops u's sign, docs\systems\assets.md)  VK_FORMAT_R16G16_SFLOAT (83)
// The file stores ONE skin pair per vertex for these meshes (S1: LE u32 palette slot + BE f32 weight 1.0). The game's vertex then holds
//   bone ids  = (slot, 255, 255, 255)   -- byte 0 is the file's palette slot itself (the vertex program indexes the palette rows c76 + 3 * id), 255 = no bone
//   weights   = (0, 0, 0, 255)          -- no weight is stored for a single pair (the wrestlers' "first (pairs - 1) pairs" rule, docs\formats\wrestler_host_vertex.md)
// Only that shape is written here (SinglePairWords refuses any other: more pairs, a slot of 255 or more, a weight other than 1.0).
// The vertex colour: our loader's R, G, B, A bytes (the file's A R G B word re-ordered) equal the stream's four bytes in the same order on every arena prop vertex [C]. The ring's
// colour is "the file's bytes reordered" (RING map 1.3, order not yet pinned by a measurement): ColourOrder names the permutation, the caller passes the measured one.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace assets::prop_host {

constexpr uint8_t kNoBone = 255;

// Planted faults (the unit test's twins: each must fail its exact check). Zero-initialised = none.
struct Faults {
  bool colour_order = false;        // the colour written in another byte order (the vertex-colour swizzle class)
  bool uv_halves_swapped = false;   // u and v exchanged
  bool bone_zero = false;           // bone id 0 for every vertex
  bool weight_word = false;         // the weight word (255, 0, 0, 0) instead of (0, 0, 0, 255)
};

// The two 32-bit words of the single-pair rule.
struct SkinWords {
  uint8_t weights[4];
  uint8_t ids[4];
};

// `slots` / `weights`: the file's pairs of one vertex, 4 entries (slot 255 or weight 0 = unused); `pairs`: how many the stream stores a vertex (file S1 stride / 8).
// True only for the rule above: exactly one stored pair, slot < 255, weight exactly 1.0, the other entries unused.
inline bool SinglePairWords(uint32_t pairs, const uint8_t slots[4], const float weights[4], const Faults& faults, SkinWords& out) {
  if (pairs != 1 || slots[0] >= kNoBone || weights[0] != 1.0f) return false;
  for (int k = 1; k < 4; ++k)
    if (slots[k] != kNoBone || weights[k] != 0.0f) return false;
  const uint8_t w[4] = {0, 0, 0, 255}, wf[4] = {255, 0, 0, 0};
  std::memcpy(out.weights, faults.weight_word ? wf : w, 4);
  out.ids[0] = faults.bone_zero ? uint8_t(0) : slots[0];
  out.ids[1] = out.ids[2] = out.ids[3] = kNoBone;
  return true;
}

// float -> half bits, TRUNCATING (the game's conversion; round-to-nearest left 5 beam-light arena objects ambiguous, ARENA_OURS.md).
inline uint16_t FloatToHalfTruncate(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
  const uint32_t m = x & 0x7FFFFFu;
  if (e >= 31) return uint16_t(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return uint16_t(sign);
    return uint16_t(sign | ((m | 0x800000u) >> (14 - e)));
  }
  return uint16_t(sign | (uint32_t(e) << 10) | (m >> 13));
}

// A unit-range float -> SNORM16 (round to nearest, clamped). The stream's normal is the game's own packed value: within one field step of this.
inline int16_t FloatToSnorm16(float v) {
  const float c = std::fmin(1.0f, std::fmax(-1.0f, v));
  return int16_t(std::lrintf(c * 32767.0f));
}

// How the colour bytes (R, G, B, A as our loaders hold them) land in the host vertex's four colour bytes: out[k] = in[perm[k]].
struct ColourOrder {
  uint8_t perm[4] = {0, 1, 2, 3};   // identity = the arena props' measured order
};
inline ColourOrder WrongOrder() { return ColourOrder{{3, 0, 1, 2}}; }   // (the planted fault: rotated, so grey colours with an opaque alpha still change)

#pragma pack(push, 1)
struct HostVertex {   // 40 bytes
  float position[4];
  uint8_t weights[4];
  uint8_t ids[4];
  int16_t normal[4];
  uint8_t colour[4];
  uint16_t uv[2];
};
#pragma pack(pop)
static_assert(sizeof(HostVertex) == 40, "the skinned prop host vertex is 40 bytes");

// The host vertex of one file vertex. `abs_u`: the stream's u is |file u| (see above); true to match the stream byte for byte, false to keep the file's sign
// (a half keeps its sign bit, so the SDK fix needs no change here).
inline HostVertex MakeHostVertex(const float pos[3], const float normal[3], const uint8_t rgba[4], const float uv[2], const SkinWords& words, bool abs_u, const ColourOrder& order,
                                 const Faults& faults) {
  HostVertex v;
  v.position[0] = pos[0], v.position[1] = pos[1], v.position[2] = pos[2], v.position[3] = 1.0f;
  std::memcpy(v.weights, words.weights, 4);
  std::memcpy(v.ids, words.ids, 4);
  for (int k = 0; k < 3; ++k) v.normal[k] = FloatToSnorm16(normal[k]);
  v.normal[3] = 0;
  const ColourOrder use = faults.colour_order ? WrongOrder() : order;
  for (int k = 0; k < 4; ++k) v.colour[k] = rgba[use.perm[k]];
  const float u = abs_u ? std::fabs(uv[0]) : uv[0];
  const uint16_t hu = FloatToHalfTruncate(u), hv = FloatToHalfTruncate(uv[1]);
  v.uv[0] = faults.uv_halves_swapped ? hv : hu;
  v.uv[1] = faults.uv_halves_swapped ? hu : hv;
  return v;
}

}  // namespace assets::prop_host
