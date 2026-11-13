// Wrestler part geometry written in the stream draw's HOST vertex layout (Session 101, lane 45 = W2; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 3-4 and 10).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure functions: no SDK, no game, no renderer includes.
//
// The game's wrestler vertex (sub_82BD4BE0, one call per vertex at load) is dwords [pos x y z w][weights x3 if 2+ skin pairs][slot bytes][normal 10:11:11 word]
// [tangent word if the file has S3][colour word][uv half2]; the capture layer converts only the elements the vertex shader FETCHES to a host vertex (reader.h
// ConvertVertex: floats as they are, 8-bit words as the swapped word, 10:11:11 to four snorm16 with w = 0, a half pair to two halves). A part's host vertex is therefore a
// pure function of the file's four streams and the layout the draw names, so ours is built straight from the file in that layout (the same bytes, positions times the
// body's one scale) and the stream's own captured bytes are the proof (VerifyAgainst).
//
// What was measured and what is a rule: see docs\renderer\WRESTLER_PROMOTION_MAP.md section 11 (lane 45). Every rule here is checked per draw against the stream's
// captured bytes in fview (and per Nth draw live); a rule that does not hold makes the draw a refusal (counted), never a wrong picture.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wrestler_roster.h"   // (it declares stb_image's zlib for pac.h, which jboy_parts.h includes)

namespace assets::wrestler_geom {

// One fetched element of the stream's host layout (the fields of the capture's VertexElement that matter here).
struct LayoutElement {
  uint16_t vk_format = 0;
  uint16_t host_offset = 0;
  uint8_t source_format = 0;   // Xenos vertex format code: 38 float4, 57 float3, 6 8888, 17 11:11:10, 31 half2
  uint8_t components = 0;
  uint8_t flags = 0;
  int32_t guest_offset = 0;    // dwords
};

enum class Role : uint8_t { Pos, Weights, Slots, Normal, Tangent, Colour, Uv, Fill0 };
inline const char* RoleName(Role r) {
  static const char* const n[] = {"pos", "weights", "slots", "normal", "tangent", "colour", "uv", "fill0"};
  return n[uint8_t(r)];
}

struct Roles {
  struct R { Role role; uint16_t offset; uint16_t vk; };
  std::vector<R> r;
  uint32_t host_stride = 0;
  std::string why;      // set when Classify refuses
  bool tangent_not_in_file = false;   // S101 lane 206: the 12-dword long map: the game fetches a tangent the file does not carry (we write zero = the file's truth; Compare skips it)
};

// The guest dword of each field of this part's vertex, from the file's stream strides (the mesh flag word's effect). -1: not present.
struct GuestMap { int weights = -1, slots = 4, normal = 5, tangent = -1, colour = 6, uv = 7, dwords = 8; };
inline GuestMap MapFor(const jboy_parts::Part& p) {
  GuestMap g;
  const uint32_t pairs = p.stride[1] / 8;
  int at = 4;
  if (pairs >= 2) g.weights = at, at += 3;
  g.slots = at++;
  g.normal = at++;
  if (p.stride[3] == 12) g.tangent = at++;
  g.colour = at++;
  g.uv = at++;
  g.dwords = at;
  return g;
}

// Names each fetched element (by its source format and guest dword) and refuses a layout the part cannot fill. `guest_stride_dwords` is the layout's own stride.
inline bool ClassifyMap(const jboy_parts::Part& p, const GuestMap& g, const LayoutElement* els, size_t n, uint32_t host_stride, uint32_t guest_stride_dwords, Roles& out) {   // lane 134: the map is a parameter
  out = {};
  out.host_stride = host_stride;
  if (!p.ok || !p.tiles) return out.why = "part not usable", false;
  if (guest_stride_dwords != uint32_t(g.dwords)) return out.why = "guest stride " + std::to_string(guest_stride_dwords) + " != " + std::to_string(g.dwords), false;
  bool has_pos = false;
  for (size_t i = 0; i < n; ++i) {
    const LayoutElement& e = els[i];
    Role role;
    switch (e.source_format) {
      case 38: if (e.guest_offset != 0) return out.why = "float4 not at 0", false; role = Role::Pos; has_pos = true; break;
      case 57: if (e.guest_offset != g.weights || g.weights < 0) return out.why = "weights where the file has none", false; role = Role::Weights; break;
      case 17:
        if (e.guest_offset == g.normal) role = Role::Normal;
        else if (g.tangent >= 0 && e.guest_offset == g.tangent) role = Role::Tangent;
        else return out.why = "10:11:11 at an unknown dword", false;
        break;
      case 31: if (e.guest_offset != g.uv) return out.why = "uv at an unknown dword", false; role = Role::Uv; break;
      case 6:
        if (e.guest_offset == g.slots) role = Role::Slots;
        else if (e.guest_offset == g.colour) role = Role::Colour;
        else if (e.guest_offset == 0) role = Role::Fill0;
        else return out.why = "8888 at an unknown dword", false;
        break;
      default: return out.why = "element format " + std::to_string(e.source_format), false;
    }
    // The host format and the element flags are part of the bytes we write: a layout variant that differs in either is counted Variant, never filled with these rules.
    uint16_t vk = 0;
    size_t bytes = 0;
    switch (role) {
      case Role::Pos: vk = 109, bytes = 16; break;
      case Role::Weights: vk = 106, bytes = 12; break;
      case Role::Slots: vk = 41, bytes = 4; break;
      case Role::Normal: case Role::Tangent: vk = 92, bytes = 8; break;
      case Role::Colour: case Role::Fill0: vk = 37, bytes = 4; break;
      case Role::Uv: vk = 83, bytes = 4; break;
    }
    if (e.vk_format != vk) return out.why = std::string("host format ") + std::to_string(e.vk_format) + " for " + RoleName(role), false;
    if (size_t(e.host_offset) + bytes > host_stride) return out.why = "element past the host stride", false;
    const bool is_signed = (e.flags & 1) != 0, is_integer = (e.flags & 2) != 0;   // ElementFlags kElementSigned / kElementInteger
    if ((role == Role::Normal || role == Role::Tangent) && !is_signed) return out.why = "unsigned 10:11:11", false;
    if (role == Role::Slots && (!is_integer || is_signed)) return out.why = "slot bytes not unsigned integer", false;
    if ((role == Role::Colour || role == Role::Fill0) && (is_integer || is_signed)) return out.why = "8888 not unsigned normalised", false;
    out.r.push_back({role, e.host_offset, e.vk_format});
  }
  if (!has_pos) return out.why = "no position element", false;
  return true;
}

// S101 lane 134: the game's per-mesh vertex declaration (flag word, sub_82BD4BE0: tangent bit 0x10000, colour bit 0x40) is not always the one the file's streams imply. A yCh_trans PIECE (the
// referee's shirt patch, the lashes of a body's alpha piece) in a file that carries S3 is drawn from an 11-dword vertex: one of tangent / colour is not written, the uv sits one dword lower.
// The vertex shader fetches neither of the two at that dword (the capture keeps only fetched elements), so which one is missing cannot change a byte we write: both variants are tried
// (tangent without colour, then colour without tangent) and the first that names every fetched element wins. A layout that matches the file's own map is classified exactly as before.
// lane 134 (follow-up): the short maps are OFF unless the caller asks (`allow_short_maps`): only the wrestlers' core sets it; props / ring / screens classify with the file's own map as before.
// `shift_fault` (a plant, wrestlers' short_map_shift): the short maps put the uv one dword too high, so the layout must stay refused.
// out.why of a refusal names every attempt: "file: ...; no_colour: ...; no_tangent: ...".
inline bool Classify(const jboy_parts::Part& p, const LayoutElement* els, size_t n, uint32_t host_stride, uint32_t guest_stride_dwords, Roles& out, bool allow_short_maps = false, bool shift_fault = false, bool allow_long_map = false) {   // lane 206: allow_long_map is the wrestlers' core only (the CAW editor, props, ring, screens keep it off)
  const GuestMap g = MapFor(p);
  if (ClassifyMap(p, g, els, n, host_stride, guest_stride_dwords, out)) return true;
  const std::string why_file = out.why;
  // S101 lane 206: the LONG map. A file with no tangent stream can still be drawn from a 12-dword vertex [... normal][tangent][colour][uv] (The Rock Retro's elbow pads, yCh_gear): the tangent dword is
  // fetched but the file carries none (the game's bytes there are zero or leftovers: never copied, rule 6). Same gate as the short maps; `shift_fault` leaves the uv at the file's dword so the layout stays refused.
  if (allow_long_map && g.tangent < 0 && guest_stride_dwords == uint32_t(g.dwords) + 1) {
    GuestMap lng = g;
    lng.tangent = g.normal + 1, lng.colour = g.colour + 1, lng.uv = g.uv + (shift_fault ? 0 : 1), lng.dwords = g.dwords + 1;
    if (ClassifyMap(p, lng, els, n, host_stride, guest_stride_dwords, out)) {
      for (const Roles::R& r : out.r) out.tangent_not_in_file = out.tangent_not_in_file || r.role == Role::Tangent;   // review S3a: the flag only when a tangent is fetched
      return true;
    }
    out.why = "file: " + why_file + "; long: " + out.why;
    return false;
  }
  if (!allow_short_maps || guest_stride_dwords + 1 != uint32_t(g.dwords) || g.tangent < 0) return false;   // (out.why is the file map's)
  GuestMap no_colour = g;   // [... normal][tangent][uv]: the colour dword is absent
  no_colour.colour = -2, no_colour.uv = g.uv - (shift_fault ? 0 : 1), no_colour.dwords = g.dwords - 1;
  if (ClassifyMap(p, no_colour, els, n, host_stride, guest_stride_dwords, out)) return true;
  const std::string why_nc = out.why;
  GuestMap no_tangent = g;  // [... normal][colour][uv]: the tangent dword is absent
  no_tangent.tangent = -1, no_tangent.colour = g.tangent, no_tangent.uv = g.uv - (shift_fault ? 0 : 1), no_tangent.dwords = g.dwords - 1;
  if (ClassifyMap(p, no_tangent, els, n, host_stride, guest_stride_dwords, out)) return true;
  out.why = "file: " + why_file + "; no_colour: " + why_nc + "; no_tangent: " + out.why;
  return false;
}

struct BuildOptions {
  float scale = 1.0f;          // the body's one position scale (measured once per object)
  bool uv_swap = false;        // planted: the two uv halves exchanged (Session 100's bug)
  uint8_t fill0 = 0;           // the dummy 8888 element at guest dword 0 (rigid parts): 0 zero, 1 {0,0,0,255}, 2 {0,0,0,high byte of position x}
  bool normal_round_nearest = true;
};

// float -> half, rounding toward zero (MEASURED on run 7051: the game's uv halves are the truncated ones; round-to-nearest is one ulp high in about 3 of 4 values).
inline uint16_t FloatToHalf(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  const uint32_t s = (b >> 16) & 0x8000u;
  const int32_t e = int32_t((b >> 23) & 0xFF) - 127 + 15;
  uint32_t m = b & 0x7FFFFFu;
  if (((b >> 23) & 0xFF) == 255) return uint16_t(s | 0x7C00u | (m ? 0x200u : 0));
  if (e >= 31) return uint16_t(s | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return uint16_t(s);
    m |= 0x800000u;
    return uint16_t(s | (m >> uint32_t(14 - e)));
  }
  return uint16_t(s | (uint32_t(e) << 10) | (m >> 13));
}

// A signed field of `bits` bits: the game's float -> integer (nearest) of value * max, then the capture layer's integer -> snorm16.
inline int16_t Snorm16FromField(float v, int bits) {
  const double mx = double((1 << (bits - 1)) - 1);  // 511 for 10 bits, 1023 for 11
  const double c = std::max(-1.0, std::min(1.0, double(v)));
  const double i = std::nearbyint(c * mx);
  const double f = std::max(i / mx, -1.0);
  const double scaled = f * 32767.0;
  return int16_t(int32_t(scaled < 0 ? scaled - 0.5 : scaled + 0.5));
}

inline void PackVec3(const float v[3], uint8_t* dst) {  // field order x:10 y:11 z:11 (reader.h ConvertVertex, source format 17), w = 0
  const int16_t q[4] = {Snorm16FromField(v[0], 10), Snorm16FromField(v[1], 11), Snorm16FromField(v[2], 11), 0};
  std::memcpy(dst, q, 8);
}

// Builds the part's whole host vertex buffer in the classified layout. `out` = part.vertices * roles.host_stride bytes.
inline void BuildVertices(const jboy_parts::Part& p, const Roles& roles, const BuildOptions& opt, std::vector<uint8_t>& out) {
  const uint32_t N = p.vertices, stride = roles.host_stride;
  out.assign(size_t(N) * stride, 0);
  for (uint32_t v = 0; v < N; ++v) {
    uint8_t* dst = out.data() + size_t(v) * stride;
    const uint8_t* s0 = p.s[0] + 28ull * v;
    const uint8_t* s1 = p.s[1] + size_t(p.stride[1]) * v;
    const uint8_t* s2 = p.s[2] + size_t(p.stride[2]) * v;
    const uint8_t* s3 = p.stride[3] ? p.s[3] + size_t(p.stride[3]) * v : nullptr;
    float pos[3];
    for (int c = 0; c < 3; ++c) pos[c] = BeF(s0 + 4 * c) * opt.scale;
    const uint32_t pairs = p.stride[1] / 8;
    for (const Roles::R& r : roles.r) {
      uint8_t* d = dst + r.offset;
      switch (r.role) {
        case Role::Pos: {
          const float one = 1.0f;
          std::memcpy(d, pos, 12);
          std::memcpy(d + 12, &one, 4);
          break;
        }
        case Role::Weights: {  // three floats: the first (pairs - 1) pairs' weights (the last pair's is implied: the shader takes 1 - the sum), 0 beyond (MEASURED, 4 / 3 / 2 pairs)
          for (uint32_t k = 0; k < 3; ++k) {
            uint32_t slot = 255;
            if (k + 1 < pairs) std::memcpy(&slot, s1 + 8 * k, 4);
            const float w = (k + 1 < pairs && slot != 255) ? BeF(s1 + 8 * k + 4) : 0.0f;  // an absent pair (slot 255) carries float noise in the file: the game writes 0
            std::memcpy(d + 4 * k, &w, 4);
          }
          break;
        }
        case Role::Slots: {
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t slot = 255;
            if (k < pairs) std::memcpy(&slot, s1 + 8 * k, 4);  // little-endian u32 in the file
            d[k] = uint8_t(slot > 255 ? 255 : slot);
          }
          break;
        }
        case Role::Normal: {
          float n[3];
          for (int c = 0; c < 3; ++c) n[c] = BeF(s0 + 12 + 4 * c);
          PackVec3(n, d);
          break;
        }
        case Role::Tangent: {
          float t[3] = {0, 0, 0};
          if (s3) for (int c = 0; c < 3; ++c) t[c] = BeF(s3 + 4 * c);
          PackVec3(t, d);
          break;
        }
        case Role::Colour: {
          const uint32_t w = Be32(s0 + 24);  // the file's big-endian word, numerically (the converter copies it)
          std::memcpy(d, &w, 4);
          break;
        }
        case Role::Uv: {
          uint16_t h[2] = {FloatToHalf(BeF(s2)), FloatToHalf(BeF(s2 + 4))};
          if (opt.uv_swap) std::swap(h[0], h[1]);
          std::memcpy(d, h, 4);
          break;
        }
        case Role::Fill0: {
          uint8_t b[4] = {0, 0, 0, 0};
          if (opt.fill0 == 1) b[3] = 255;
          else if (opt.fill0 == 2) {
            uint32_t xb;
            std::memcpy(&xb, pos, 4);
            b[3] = uint8_t(xb >> 24);
          }
          std::memcpy(d, b, 4);
          break;
        }
      }
    }
  }
}

// The part's index buffer: the file's strip (all groups, in order) as host u16.
// lane 179: a part with SEVERAL index groups is drawn as ONE strip with a 0xFFFF restart marker between the groups (measured on the editor's part 4, ed02 leaf 2002 mesh 1: groups 685 + 678, draw
// count 1364 = 685 + 1 + 678, stream index = group 0, 0xFFFF, group 1, value for value). The draw's index count is therefore sum(groups) + groups - 1. A single-group part is unchanged
// (count = its index count, indices = its group). `flat` is the PLANT (the old rule: plain sum, no markers).
inline uint32_t DrawIndexCount(const jboy_parts::Part& p, bool flat = false) { return p.index_count + (!flat && p.index_groups > 1 ? p.index_groups - 1 : 0); }
inline std::vector<uint16_t> BuildIndices(const jboy_parts::Part& p, bool flat = false) {
  if (flat) return jboy_parts::AllIndices(p);
  std::vector<uint16_t> r;
  for (size_t g = 0; g < p.groups.size(); ++g) {
    if (g) r.push_back(0xFFFF);
    for (uint32_t i = 0; i < p.groups[g].count; ++i) r.push_back(Be16(p.groups[g].data + 2ull * i));
  }
  return r;
}

// ---- comparison with the stream's captured bytes ------------------------------------------------------------------------------
struct RoleCompare {
  uint64_t values = 0, exact = 0, near = 0, bad = 0;   // per vertex element: bit-equal / within the role's tolerance / beyond it
  double max_err = 0;
};
struct CompareResult {
  RoleCompare role[8];
  bool all_ok = true;      // every element of every vertex within tolerance
  uint32_t first_bad_vertex = 0xFFFFFFFFu;
  Role first_bad_role = Role::Pos;
};
// Tolerances: positions a few float ulps of the scaled value; everything else must be bit-equal except the normals' and tangents' last snorm step (one integer
// step of the game's rounding is not a different picture and would show only if the rounding rule were wrong in general, which `bad` counts).
// lane 179 (CAW editor): `fill0_follows_pos` accepts a differing fill0 byte when the STREAM's fill0 byte is the high byte of the stream's OWN position x (the rule the game's fetch patch
// follows, mode 2 of Fill0Mode) and our x is within the position tolerance of the stream's: a vertex near x = 0 moved by 1e-5 changes that byte (sign / exponent) without any picture
// difference (the vertex shaders do not use fill0 for a rigid part). Default false: the wrestlers' class compares fill0 bit-exactly as before.
inline void Compare(const Roles& roles, const std::vector<uint8_t>& ours, const uint8_t* theirs, uint32_t vertex_count, CompareResult& res, float pos_tolerance_rel = 2e-6f, bool fill0_follows_pos = false) {
  const uint32_t stride = roles.host_stride;
  int pos_offset = -1;
  for (const Roles::R& r : roles.r) if (r.role == Role::Pos) pos_offset = int(r.offset);
  uint64_t tan_total = 0, tan_zero = 0;
  uint32_t tan_first_v = 0xFFFFFFFFu;
  for (uint32_t v = 0; v < vertex_count; ++v) {
    for (const Roles::R& r : roles.r) {
      const uint8_t* a = ours.data() + size_t(v) * stride + r.offset;
      const uint8_t* b = theirs + size_t(v) * stride + r.offset;
      RoleCompare& rc = res.role[uint8_t(r.role)];
      size_t bytes = 4;
      switch (r.vk) { case 109: bytes = 16; break; case 106: bytes = 12; break; case 92: bytes = 8; break; default: bytes = 4; }
      ++rc.values;
      if (r.role == Role::Tangent && roles.tangent_not_in_file) {   // review S2: the exemption needs the stream's tangents to be (mostly) zero, else the long map is not what this part is
        static const uint8_t kZero8[8] = {};
        ++tan_total, tan_zero += !std::memcmp(b, kZero8, 8);
        if (tan_first_v == 0xFFFFFFFFu) tan_first_v = v;
      }
      if (!std::memcmp(a, b, bytes)) { ++rc.exact; continue; }
      if (r.role == Role::Tangent && roles.tangent_not_in_file) { ++rc.near; continue; }   // lane 206: the file has no tangent; the game's bytes there are leftovers, not data
      bool near = false;
      double err = 0;
      if (r.role == Role::Pos) {
        near = true;
        for (int c = 0; c < 3; ++c) {
          float fa, fb;
          std::memcpy(&fa, a + 4 * c, 4), std::memcpy(&fb, b + 4 * c, 4);
          const double d = std::fabs(double(fa) - double(fb)), tol = double(pos_tolerance_rel) * std::max(1.0, std::fabs(double(fb)));
          err = std::max(err, d);
          if (!(d <= tol)) near = false;
        }
        if (std::memcmp(a + 12, b + 12, 4)) near = false;
      } else if (r.role == Role::Fill0 && fill0_follows_pos && pos_offset >= 0) {   // lane 179
        float xo, xt;
        std::memcpy(&xo, ours.data() + size_t(v) * stride + pos_offset, 4), std::memcpy(&xt, theirs + size_t(v) * stride + pos_offset, 4);
        uint32_t bt;
        std::memcpy(&bt, &xt, 4);
        const double d = std::fabs(double(xo) - double(xt));
        near = a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && b[3] == uint8_t(bt >> 24) && d <= double(pos_tolerance_rel) * std::max(1.0, std::fabs(double(xt)));
        err = d;
      } else if (r.role == Role::Normal || r.role == Role::Tangent) {
        near = true;
        for (int c = 0; c < 4; ++c) {
          int16_t qa, qb;
          std::memcpy(&qa, a + 2 * c, 2), std::memcpy(&qb, b + 2 * c, 2);
          const int d = std::abs(int(qa) - int(qb));
          err = std::max(err, double(d));
          if (d > 70) near = false;   // ~2 field steps of the 10-bit field (32767 / 511 = 64): the rounding rule, not a different vector
        }
      }
      rc.max_err = std::max(rc.max_err, err);
      if (near) ++rc.near;
      else {
        ++rc.bad;
        if (res.all_ok) res.first_bad_vertex = v, res.first_bad_role = r.role;
        res.all_ok = false;
      }
    }
  }
  // review S2 (lane 206): the tangent exemption stands only when most stream tangents are zero (the 7093 elbow pads: 88%); a part whose stream carries real tangents is not this layout
  if (tan_total && tan_zero * 2 < tan_total) {
    RoleCompare& rc = res.role[uint8_t(Role::Tangent)];
    rc.bad += tan_total - tan_zero;
    if (res.all_ok) res.first_bad_vertex = tan_first_v, res.first_bad_role = Role::Tangent;
    res.all_ok = false;
  }
}

// The one scale of a body: the per-axis least-squares fit of the stream's positions on the file's, averaged over the axes, then the float whose product matches the
// most vertices (a few ulps around the fit). Returns the scale and the fit's relative residual (rms / extent); 0 when nothing could be fitted.
inline float FitScale(const jboy_parts::Part& p, const Roles& roles, const uint8_t* theirs, double* rel_residual = nullptr) {
  uint16_t pos_off = 0xFFFF;
  for (const Roles::R& r : roles.r) if (r.role == Role::Pos) pos_off = r.offset;
  if (pos_off == 0xFFFF || !p.ok) return 0;
  double ab = 0, bb = 0;
  for (uint32_t v = 0; v < p.vertices; ++v)
    for (int c = 0; c < 3; ++c) {
      float g;
      std::memcpy(&g, theirs + size_t(v) * roles.host_stride + pos_off + 4 * c, 4);
      const double b = BeF(p.s[0] + 28ull * v + 4 * c);
      ab += double(g) * b, bb += b * b;
    }
  if (bb <= 0) return 0;
  const float fit = float(ab / bb);
  float best = fit;
  uint32_t best_hits = 0;
  uint32_t bits;
  std::memcpy(&bits, &fit, 4);
  for (int d = -6; d <= 6; ++d) {
    const uint32_t cb = bits + uint32_t(d);
    float cand;
    std::memcpy(&cand, &cb, 4);
    uint32_t hits = 0;
    for (uint32_t v = 0; v < p.vertices; ++v)
      for (int c = 0; c < 3; ++c) {
        float g;
        std::memcpy(&g, theirs + size_t(v) * roles.host_stride + pos_off + 4 * c, 4);
        hits += (BeF(p.s[0] + 28ull * v + 4 * c) * cand == g);
      }
    if (hits > best_hits) best_hits = hits, best = cand;
  }
  if (rel_residual) {
    double ss = 0, ext = 1e-9;
    for (uint32_t v = 0; v < p.vertices; ++v)
      for (int c = 0; c < 3; ++c) {
        float g;
        std::memcpy(&g, theirs + size_t(v) * roles.host_stride + pos_off + 4 * c, 4);
        const double b = BeF(p.s[0] + 28ull * v + 4 * c) * double(best);
        ss += (double(g) - b) * (double(g) - b);
        ext = std::max(ext, std::fabs(b));
      }
    *rel_residual = std::sqrt(ss / (3.0 * p.vertices)) / ext;
  }
  return best;
}

}  // namespace assets::wrestler_geom
