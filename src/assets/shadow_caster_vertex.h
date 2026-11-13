// The shadow-pass caster vertex, written in the stream draw's SHADOW host layout (Session 101, lane 85 = SV; docs\renderer\SHADOW_PROMOTION_MAP.md sections 1.3 / 7,
// docs\systems\shadows.md "Measured (lane SC)" and "Shadow vertex writers (lane SV)"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no SDK, no game data.
//
// The shadow effects (yShadowMap / yPlaneShadowMap) fetch a SUBSET of the model's converted vertex, with their own declaration, so a caster draw's host layout is one of four shapes
// (Check B 7051 and run 7079, every shadow draw; elements by [host offset: Xenos source format, guest dword, mini-fetch bit]):
//   wrestler skinned   stride 32 / guest 12   pos f32x4 @0, weights f32x3 @16 (g4), ids u8x4 UINT @28 (g7)
//   rigid part / prop  stride 24 / guest 8    pos @0, FILL u8x4 @16 (g0, mini 0), ids @20 (g4)
//   ring proxy 956     stride 28 / guest 8    pos, FILL @16 (g0, mini 0), ids @20, XDWORD u8x4 @24 (g0, mini 1)
//   chCommon proxy     stride 36 / guest 11   pos, weights @16 (g4), ids @28 (g7), XDWORD @32 (g0, mini 1)
// The rules (all measured byte for byte on every vertex of those draws, tools\shadow\caster_vertex_check.cpp):
//   position  file x y z times the body's one scale (1.0 for the ring and the proxies), w = 1
//   weights   f32x3 = the first (pairs - 1) skin pairs' weights, 0 for an absent pair (wrestler_geom.h BuildVertices' rule: the shadow vertex is a byte-equal subset of the colour vertex)
//   ids       u8x4 = (slot of pair 0, ..., 255 where absent): the file's palette slot itself
//   FILL      the u8x4 read at guest dword 0 by a rigid part's declaration: a wrestler / prop part (0, 0, 0, high byte of the scaled position x float); the ring (single skin pair,
//             prop_host_vertex.h's weight word) (0, 0, 0, 255). Which one is the caller's kind (FillMode), learned once per object from the stream, never from the layout.
//   XDWORD    the plane proxies' fourth element, a MINI fetch of guest dword 0: (0, bytes 1..3 of the position x float) = the float's four memory bytes with the low byte zeroed.
//             It is NOT the vertex colour (a mini fetch reads the position's own dword).
//   uv        (no shadow layout carries one; the colour layout's uv element is written by the same PackUv) halves truncated toward zero of (u, v); u is |file u| in streams taken before SDK
//             patch 109 and the signed file u after it: LEARNED from the stream's own bytes (LearnUvSign), never assumed; a mesh with a negative u and an unknown sign is refused.
// Planted faults (Faults): the real past bugs this writer must not repeat; each fails its own exact check in the unit test.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "jboy_parts.h"
#include "prop_host_vertex.h"   // FloatToHalfTruncate, SinglePairWords (read only)
#include "wrestler_geom.h"      // (the wrestler host vertex: read only; includes pac.h's BeF)

namespace assets::shadow_vertex {

struct Faults {
  bool fetch_order = false;       // R (S100 08:29): the dword-0 u8x4 written in the file's (big-endian) byte order instead of the fetched one: "8-bit garbage as positions"
  bool weights_as_fill = false;   // R (ring.md B5 correction 1): the fill word (0,0,0,255) written where the layout has the f32x3 weights
  bool xdword_colour = false;     // R (ring.md B5 correction 3): the file's vertex colour in the plane proxies' dword-0 element
  bool bone_zero = false;         // every bone id 0: the map collapses
  bool uv_sign = false;           // R (patch 109): the u sign rule written the wrong way round for the stream at hand
};

enum class Role : uint8_t { Pos, Weights, Ids, Fill, XDword, Uv };
inline const char* RoleName(Role r) {
  static const char* const n[] = {"pos", "weights", "ids", "fill", "xdword", "uv"};
  return n[uint8_t(r)];
}

// One element of the stream's host layout (the fields of the capture's VertexElement that matter here; ElementFlags: 1 signed, 2 integer).
struct Element {
  uint16_t vk_format = 0;
  uint16_t host_offset = 0;
  uint8_t source_format = 0;   // Xenos vertex format code: 38 float4, 57 float3, 6 8888, 31 half2
  uint8_t flags = 0;
  uint8_t mini = 0;            // a mini fetch in the microcode
  int32_t guest_offset = 0;    // dwords
};

struct Layout {
  struct R { Role role; uint16_t offset; };
  std::vector<R> r;
  uint32_t host_stride = 0;
  std::string why;   // set when Classify refuses
  bool Has(Role x) const {
    for (const R& e : r) if (e.role == x) return true;
    return false;
  }
};

enum class FillMode : uint8_t { Weight255, HiByteX };   // see the header comment
enum class UvSign : uint8_t { Unknown, Abs, Signed };

// Names each element and refuses a layout the part cannot fill. A shadow layout holds only elements of the six roles; anything else (normal, tangent, colour: a colour-pass layout) is refused.
inline bool Classify(const jboy_parts::Part& p, const Element* els, size_t n, uint32_t host_stride, uint32_t guest_stride_dwords, Layout& out) {
  out = {};
  out.host_stride = host_stride;
  if (!p.ok || !p.tiles) return out.why = "part not usable", false;
  const wrestler_geom::GuestMap g = wrestler_geom::MapFor(p);
  (void)guest_stride_dwords;   // (the shadow declaration's own guest stride is not the colour vertex's: a part with a tangent shows 8 where the file map says 9; the element dwords below are the check)
  bool has_pos = false;
  for (size_t i = 0; i < n; ++i) {
    const Element& e = els[i];
    Role role;
    size_t bytes;
    uint16_t vk;
    switch (e.source_format) {
      case 38: if (e.guest_offset != 0) return out.why = "float4 not at 0", false; role = Role::Pos, vk = 109, bytes = 16, has_pos = true; break;
      case 57: if (g.weights < 0 || e.guest_offset != g.weights) return out.why = "weights where the file has none", false; role = Role::Weights, vk = 106, bytes = 12; break;
      case 31: if (e.guest_offset != g.uv) return out.why = "uv at an unknown dword", false; role = Role::Uv, vk = 83, bytes = 4; break;
      case 6:
        if (e.flags & 2) {   // integer: the bone ids
          if (e.guest_offset != g.slots || (e.flags & 1)) return out.why = "ids at an unknown dword / signed", false;
          role = Role::Ids, vk = 41, bytes = 4;
        } else if (e.guest_offset == 0 && !(e.flags & 1)) {
          role = e.mini ? Role::XDword : Role::Fill, vk = 37, bytes = 4;   // mini fetch of dword 0 = the xdword; the plain fetch = the fill (measured, 4 layouts)
        } else return out.why = "8888 at an unknown dword (a colour-pass layout)", false;
        break;
      default: return out.why = "element format " + std::to_string(e.source_format) + " is not a shadow element", false;
    }
    if (e.vk_format != vk) return out.why = "host format " + std::to_string(e.vk_format) + " for " + RoleName(role), false;
    if (size_t(e.host_offset) + bytes > host_stride) return out.why = "element past the host stride", false;
    out.r.push_back({role, e.host_offset});
  }
  if (!has_pos) return out.why = "no position element", false;
  return true;
}

struct Options {
  float scale = 1.0f;                       // the body's one position scale
  FillMode fill = FillMode::HiByteX;
  UvSign uv_sign = UvSign::Unknown;         // learned (LearnUvSign)
};

// True when the mesh has a vertex whose file u is negative: such a mesh's uv bytes depend on the stream's sign rule.
inline int FirstNegativeU(const jboy_parts::Part& p) {
  for (uint32_t v = 0; v < p.vertices; ++v)
    if (std::signbit(BeF(p.s[2] + 8ull * v))) return int(v);   // lane 220: the sign bit (-0.0 counts)
  return -1;
}

// Learns the stream's u-sign rule from its own bytes: the half of u at the first vertex whose file u is negative carries the sign bit (Signed) or not (Abs). `vb`: the stream draw's vertex
// buffer (vertex_count * stride), `uv_offset`: the uv element's host offset. Unknown when the mesh has no negative u or the buffer is short (never guessed).
inline UvSign LearnUvSign(const jboy_parts::Part& p, const uint8_t* vb, size_t vb_bytes, uint32_t stride, uint32_t uv_offset) {
  const int v = FirstNegativeU(p);
  if (v < 0 || size_t(v + 1) * stride > vb_bytes) return UvSign::Unknown;
  uint16_t h;
  std::memcpy(&h, vb + size_t(v) * stride + uv_offset, 2);
  if (h & 0x8000u) return UvSign::Signed;
  // lane 220 follow-up: a clear bit teaches |u| only at a STRICTLY negative vertex (a -0.0 vertex whose bit the game cleared must not)
  for (uint32_t w = uint32_t(v); w < p.vertices; ++w)
    if (BeF(p.s[2] + 8ull * w) < 0.0f) {
      if (size_t(w + 1) * stride > vb_bytes) return UvSign::Unknown;
      std::memcpy(&h, vb + size_t(w) * stride + uv_offset, 2);
      return (h & 0x8000u) ? UvSign::Signed : UvSign::Abs;
    }
  return UvSign::Unknown;
}

// The uv element of one vertex: halves truncated toward zero of (u or |u|, v). The caller refuses an Unknown sign on a mesh with a negative u.
inline void PackUv(float u, float v, UvSign sign, bool plant_sign, uint8_t* d) {
  const bool signed_u = (sign == UvSign::Signed) != plant_sign;
  const uint16_t h[2] = {prop_host::FloatToHalfTruncate(signed_u ? u : std::fabs(u)), prop_host::FloatToHalfTruncate(v)};
  std::memcpy(d, h, 4);
}

// The whole vertex buffer of the part in the classified layout (`out` = part.vertices * layout.host_stride bytes). False: a vertex the rules do not cover (a skin slot of 255+ on a
// stored pair, a Signed/Abs question left open): nothing is written and the draw stays the stream's.
inline bool Write(const jboy_parts::Part& p, const Layout& lay, const Options& opt, const Faults& faults, std::vector<uint8_t>& out) {
  const uint32_t N = p.vertices, stride = lay.host_stride, pairs = p.stride[1] / 8;
  if (lay.Has(Role::Uv) && opt.uv_sign == UvSign::Unknown && FirstNegativeU(p) >= 0) return false;
  out.assign(size_t(N) * stride, 0);
  for (uint32_t v = 0; v < N; ++v) {
    uint8_t* dst = out.data() + size_t(v) * stride;
    const uint8_t* s0 = p.s[0] + 28ull * v;
    const uint8_t* s1 = p.s[1] + size_t(p.stride[1]) * v;
    const uint8_t* s2 = p.s[2] + size_t(p.stride[2]) * v;
    float pos[4] = {BeF(s0) * opt.scale, BeF(s0 + 4) * opt.scale, BeF(s0 + 8) * opt.scale, 1.0f};
    uint32_t xb;
    std::memcpy(&xb, pos, 4);
    uint8_t slot_bytes[4] = {255, 255, 255, 255};
    float pair_w[4] = {0, 0, 0, 0};
    for (uint32_t k = 0; k < 4 && k < pairs; ++k) {
      uint32_t slot;
      std::memcpy(&slot, s1 + 8 * k, 4);
      slot_bytes[k] = uint8_t(slot > 255 ? 255 : slot);
      pair_w[k] = BeF(s1 + 8 * k + 4);
    }
    for (const Layout::R& r : lay.r) {
      uint8_t* d = dst + r.offset;
      switch (r.role) {
        case Role::Pos: std::memcpy(d, pos, 16); break;
        case Role::Weights: {
          if (faults.weights_as_fill) {   // planted: the fill word where the weights go
            const uint8_t w[4] = {0, 0, 0, 255};
            std::memset(d, 0, 12), std::memcpy(d, w, 4);
            break;
          }
          for (uint32_t k = 0; k < 3; ++k) {
            const float w = (k + 1 < pairs && slot_bytes[k] != 255) ? pair_w[k] : 0.0f;   // an absent pair carries float noise in the file: the game writes 0
            std::memcpy(d + 4 * k, &w, 4);
          }
          break;
        }
        case Role::Ids:
          for (uint32_t k = 0; k < 4; ++k) d[k] = faults.bone_zero ? uint8_t(0) : slot_bytes[k];
          break;
        case Role::Fill: {
          uint8_t b[4] = {0, 0, 0, 0};
          if (opt.fill == FillMode::Weight255) b[3] = 255;
          else b[3] = uint8_t(xb >> 24);
          if (faults.fetch_order) std::swap(b[0], b[3]), std::swap(b[1], b[2]);
          std::memcpy(d, b, 4);
          break;
        }
        case Role::XDword: {
          uint8_t b[4] = {0, uint8_t(xb >> 8), uint8_t(xb >> 16), uint8_t(xb >> 24)};
          if (faults.xdword_colour) {   // planted: the file's vertex colour (A R G B word as R G B A bytes)
            const uint8_t c[4] = {s0[25], s0[26], s0[27], s0[24]};
            std::memcpy(b, c, 4);
          } else if (faults.fetch_order) std::swap(b[0], b[3]), std::swap(b[1], b[2]);
          std::memcpy(d, b, 4);
          break;
        }
        case Role::Uv: PackUv(BeF(s2), BeF(s2 + 4), opt.uv_sign, faults.uv_sign, d); break;
      }
    }
  }
  return true;
}

}  // namespace assets::shadow_vertex
