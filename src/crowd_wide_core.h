// wwe2k14 - Session 101, lane 176: the stands crowd's screen test, widened to the DISPLAYED aspect. The pure part (no SDK, no game types):
// the scale rule and the three edits the hooks make to the game's own test. Hooks and switch: src\crowd_wide.h. Why and the evidence:
// docs\renderer\CROWD_WIDE.md.
//
// The game decides which stands members exist this frame in two places, both a projection of the member against the CAMERA's matrices
// (camera block = [[0x83929A68] + 256], projection at +176, second matrix at +240) and a compare with the clip range [-1, 1]:
//   sub_8250DC10  (via sub_8250E400, once per update): one byte per crowd SECTION and side (obj + 6176 + 2 s + side): 1 = the section's
//                 centre point, projected, with its radius, touches x in [-1, 1]; x only. 0 = the whole section's members are culled.
//   sub_8250E110  (per update, 20 types x 768 members): the member (x, y - K, z) projected, its radius scaled by m00 K / m11 K, tested
//                 against x and y in [-1, 1]; a miss sets the member's 0x80000000 flag (the blocks, the far cards and the 3D groups all
//                 skip a flagged member).
// Our display shows the game's clip x / k (SDK gpu_wide_factor, ultrawide.h) and clip y / t (gpu_tall_factor). The cull has to keep every
// member inside THAT view: the test's x must be scaled by 1/k (y by 1/t), i.e. column 0 of the product matrix and the radius scale m00 K.
// The picture's own matrices are never touched: only the test's stack copies.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace wwe2k14::crowd_wide {

// 1/k and 1/t. (1, 1) = the game's own test, byte for byte (the hooks return before any write).
struct Scale {
  float x = 1.0f, y = 1.0f;
  bool On() const { return x != 1.0f || y != 1.0f; }
};

// The factors the display squeeze uses (gpu_wide_factor k >= 1, gpu_tall_factor t >= 1). Anything not clearly wider than 1.001, or absurd,
// leaves that axis alone (the same window reader.h applies to the cvars: 1.001 < f < 8).
inline Scale FromFactors(double wide, double tall) {
  Scale s;
  if (wide > 1.001 && wide < 8.0) s.x = float(1.0 / wide);
  if (tall > 1.001 && tall < 8.0) s.y = float(1.0 / tall);
  return s;
}

// The switch's text: "off" (the game's 16:9 tests), "auto" (the display's factors, the SDK cvars), or "k=<wide>,t=<tall>" (explicit factors: what a recording
// carries, so a play-back runs the same test whatever its own display is).
struct Factors {
  double w = 1.0, t = 1.0;
};
inline Factors FactorsFromSwitch(const std::string& mode, double gpu_wide, double gpu_tall) {
  Factors f;
  if (mode == "off") return f;
  double w = gpu_wide, t = gpu_tall;  // "auto" (and anything else)
  if (mode.rfind("k=", 0) == 0 && std::sscanf(mode.c_str(), "k=%lf,t=%lf", &w, &t) != 2) return f;  // unreadable: the game's own test
  if (w > 1.001 && w < 8.0) f.w = w;
  if (t > 1.001 && t < 8.0) f.t = t;
  return f;
}
inline Scale FromSwitch(const std::string& mode, double gpu_wide, double gpu_tall) {
  const Factors f = FactorsFromSwitch(mode, gpu_wide, gpu_tall);
  return FromFactors(f.w, f.t);
}

// What a recording writes for the switch: the RESOLVED state. "off" when nothing is widened, else explicit factors that FromSwitch turns back into the
// same Scale (the factors that count, formatted with 9 significant digits).
inline std::string RecordValue(const std::string& mode, double gpu_wide, double gpu_tall) {
  const Factors f = FactorsFromSwitch(mode, gpu_wide, gpu_tall);
  if (f.w == 1.0 && f.t == 1.0) return "off";
  char buf[64];
  std::snprintf(buf, sizeof buf, "k=%.9g,t=%.9g", f.w, f.t);
  return buf;
}

// Big-endian guest floats (the stack frame of the game's function, as the recompiled code left it).
inline float LoadBeF(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StoreBeF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  std::memcpy(p, &v, 4);
}

// One column of a 4x4 matrix held as 4 rows of 16 bytes at `rows` (a row vector times the matrix: output component c = sum over rows of
// v_row * m[row][c], so a column scales that output component of every point).
inline void ScaleColumn(uint8_t* rows, int col, float s) {
  for (int r = 0; r < 4; ++r) {
    uint8_t* p = rows + 16 * r + 4 * col;
    StoreBeF(p, LoadBeF(p) * s);
  }
}

// The product matrix of the member test (x and y components). x = column 0, y = column 1.
inline void ScaleProduct(uint8_t* rows, const Scale& s) {
  if (s.x != 1.0f) ScaleColumn(rows, 0, s.x);
  if (s.y != 1.0f) ScaleColumn(rows, 1, s.y);
}

// A radius scale register (sub_8250E110's f29 = m00 K, f28 = m11 K; sub_8250DC10's raw m00 at stack + 96): a single-precision multiply.
inline float ScaleRadius(float v, float s) { return v * s; }

// The two edits, on the host view `sp` of the game function's stack frame (guest r1), exactly what the midasm hooks do.
// sub_8250E110 at 0x8250E1D0: the product at sp + 112 (camera +240 times camera +176, four rows of 16 bytes), f29 = m00 K, f28 = m11 K.
inline constexpr uint32_t kMemberProduct = 112;
inline void ApplyMember(uint8_t* sp, const Scale& s, float& f28, float& f29) {
  if (!s.On()) return;
  ScaleProduct(sp + kMemberProduct, s);
  if (s.x != 1.0f) f29 = ScaleRadius(f29, s.x);
  if (s.y != 1.0f) f28 = ScaleRadius(f28, s.y);
}
// The native copy of sub_8250E110 (src/nf/frustum_cull.h, --wwe2k14_nf_frustum_cull) applies the SAME scale on its own float arrays instead of the guest
// stack: these two are what it calls (the unit test compares them with the hook's edit, ApplyMember, bit for bit). Lane 203.
// Radius scales: sx / sy hold float(m00 K) / float(m11 K) in a double; the hook's f29 / f28 get float * float.
inline void NativeRadius(double& sx, double& sy, const Scale& s) {
  if (s.x != 1.0f) sx = double(float(sx) * s.x);
  if (s.y != 1.0f) sy = double(float(sy) * s.y);
}
// The product matrix (a row vector times it): column 0 by 1/k, column 1 by 1/t, single-precision multiplies like ScaleColumn.
inline void NativeProduct(float prod[4][4], const Scale& s) {
  for (int j = 0; j < 4; ++j) {
    if (s.x != 1.0f) prod[j][0] = prod[j][0] * s.x;
    if (s.y != 1.0f) prod[j][1] = prod[j][1] * s.y;
  }
}

// sub_8250DC10 at 0x8250DC94: the product at sp + 160, the unmultiplied camera matrix at sp + 96 (its m00 is the radius scale, loaded later at
// 0x8250DCA4). The section test is x only.
inline constexpr uint32_t kSectionProduct = 160, kSectionRaw = 96;
inline void ApplySection(uint8_t* sp, const Scale& s) {
  if (s.x == 1.0f) return;
  ScaleColumn(sp + kSectionProduct, 0, s.x);
  StoreBeF(sp + kSectionRaw, ScaleRadius(LoadBeF(sp + kSectionRaw), s.x));
}

// sub_8250E7E0 (the seated floor people, called from sub_82519508 at 0x82519578 every draw) at 0x8250E858: the projection (camera +176) was just
// copied to sp + 96 (four rows of 16 bytes) and has NOT been multiplied by the view yet (sub_822505B8 at 0x8250E86C does that in place), and the
// radius scales f29 = m00 K / f28 = m11 K are loaded from it AFTER this point (lfs f13,96(r1) / lfs f12,116(r1)). So scaling column 0 / 1 of the
// copy by 1/k, 1/t widens the product and the radius scale at once.
inline constexpr uint32_t kFloorProjection = 96;
inline void ApplyFloor(uint8_t* sp, const Scale& s) {
  if (!s.On()) return;
  ScaleProduct(sp + kFloorProjection, s);
}

// Lane 234: the STAGE visibility frustum (the arena's per-node screen test: the barricade corners, the stage slots; the object published at 0x837689DC, built every update by
// sub_82769B98 from the camera's near / far / fov, 8 corners of 16 bytes at `this + 16`, x y z w as big-endian floats). The corners are built in the camera's own space with a
// horizontal half-extent from tan(fov x the game's aspect term): a 16:9 box whatever the display. Hook 0x82769D10, after the last corner store (0x82769D08) and before the loop that
// carries the corners to world space (0x82769D20) and builds the six planes (sub_82769678): corner x x k, corner y x t, so the planes enclose the DISPLAYED view.
// (The crowd's edits above scale the test's x by 1/k; this frustum is built in extents, so the same factor is applied as a multiply by k.)
inline void ApplyStageFrustum(uint8_t* corners, const Scale& s) {
  if (!s.On()) return;
  const float kx = s.x != 1.0f ? 1.0f / s.x : 1.0f, ky = s.y != 1.0f ? 1.0f / s.y : 1.0f;
  for (int j = 0; j < 8; ++j) {
    uint8_t* c = corners + 16 * j;
    if (kx != 1.0f) StoreBeF(c + 0, LoadBeF(c + 0) * kx);
    if (ky != 1.0f) StoreBeF(c + 4, LoadBeF(c + 4) * ky);
  }
}

}  // namespace wwe2k14::crowd_wide
