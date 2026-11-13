// The small GPU-only passes' meshes, as formulas (Session 101, lane 135; docs\renderer\SMALL_PASSES.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// Two passes draw static meshes the game builds from a rule, never from a file: the sweat map's disc patch (yLiquidMap, 187 vertices) and the skin pre-light's six cube faces (yPreLight,
// 726 vertices). Both are written here as the rule, GPU-free and header-only; nothing is copied from the game. The rules were fitted to the stream's bytes (tools\small\small_formula.py:
// max difference 4e-7 on Check B) and every claimed draw is compared against them (SmallPassesOurs::Replace). No game or SDK includes.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdr_scene::small_passes {

inline constexpr uint32_t kSweatCopyA = 0x8279A19Cu, kSweatCopyB = 0x82799CB4u, kSweatApply = 0x82799DC8u, kPreLight = 0x82C20B90u;   // the call sites of sub_82799B60 x3 / sub_82C20AB8 [C: names.h origins]

// ---- the sweat patch: 11 rings x 17 points (16 segments + the closing point), one strip per ring pair --------------------------------------------------------------------
inline constexpr uint32_t kDiscRings = 11, kDiscPoints = 17, kDiscVertices = kDiscRings * kDiscPoints, kDiscIndices = (kDiscRings - 1) * (kDiscPoints * 2 + 1);
inline constexpr uint32_t kCubeFaces = 6, kCubeGrid = 11, kCubeVertices = kCubeFaces * kCubeGrid * kCubeGrid, kCubeIndices = kCubeFaces * (kCubeGrid - 1) * (kCubeGrid * 2 + 1);
inline constexpr uint16_t kRestart = 0xFFFFu;

struct DiscVertex {      // vertex layout: fetch 0 = float2 (inPatch), fetch 1 = float4 (inFade; the pixel stage reads z and w), 24 bytes
  float patch[2];
  float fade[4];
};
static_assert(sizeof(DiscVertex) == 24, "disc vertex");

struct CubeVertex {      // fetch 0 = float3 position in tile pixels (y down from 0), fetch 1 = float3 direction (unit), 24 bytes
  float pos[3];
  float dir[3];
};
static_assert(sizeof(CubeVertex) == 24, "cube vertex");

// Rings 0..9 are circles of radius R = 0.5 sin^2(pi s / 2), s = ring / 9, around (0.5, 0.5), the angle running clockwise on screen from +x in 22.5 degree steps; the strength is
// w = 1 - (1 - 2R)^5 (0 at the centre, 1 at the rim). Ring 10 is the outer square (a fixed outline, strength 0): the corners of the unit patch.
inline void BuildDisc(std::vector<DiscVertex>& out) {
  out.assign(kDiscVertices, DiscVertex{});
  const double pi = 3.14159265358979323846;
  for (uint32_t r = 0; r < kDiscRings - 1; ++r) {
    const double s = double(r) / 9.0, sn = std::sin(pi * s / 2.0), t = sn * sn, radius = 0.5 * t, w = 1.0 - std::pow(1.0 - t, 5.0);
    for (uint32_t k = 0; k < kDiscPoints; ++k) {
      const double a = double(k) * 2.0 * pi / 16.0;
      DiscVertex& v = out[r * kDiscPoints + k];
      v.patch[0] = float(0.5 + radius * std::cos(a)), v.patch[1] = float(0.5 - radius * std::sin(a));
      v.fade[3] = float(w);
    }
  }
  static const float kSquare[kDiscPoints][2] = {{1, .5f}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
  for (uint32_t k = 0; k < kDiscPoints; ++k) {
    DiscVertex& v = out[(kDiscRings - 1) * kDiscPoints + k];
    v.patch[0] = kSquare[k][0], v.patch[1] = kSquare[k][1];
  }
}

// One strip per ring pair: (ring k point c, ring k+1 point c) for every point, then the restart.
inline void BuildDiscIndices(std::vector<uint16_t>& out) {
  out.clear();
  for (uint32_t k = 0; k + 1 < kDiscRings; ++k) {
    for (uint32_t c = 0; c < kDiscPoints; ++c) out.push_back(uint16_t(k * kDiscPoints + c)), out.push_back(uint16_t((k + 1) * kDiscPoints + c));
    out.push_back(kRestart);
  }
}

// The cube faces (+x, -x, +y, ... in the game's order, the corners below): each face is a kCubeGrid x kCubeGrid grid of `tile` x `tile` pixels laid side by side in one row (face f at x = tile * f),
// y running down from 0; the direction is the normalised bilinear blend of the face's three corners (top-left, top-right, bottom-left), a cube of half-size 1.
// `tile` is the target's face size in the game's pixel units (16: a 384x64 target = 6 faces of 64 px, the vertex stage's ScreenScale maps 96x16 units onto the target).
inline void BuildCube(std::vector<CubeVertex>& out, float tile = 16.0f) {
  static const int kCorners[kCubeFaces][3][3] = {{{1, 1, 1}, {1, 1, -1}, {1, -1, 1}},   {{-1, 1, -1}, {-1, 1, 1}, {-1, -1, -1}}, {{-1, 1, -1}, {1, 1, -1}, {-1, 1, 1}},
                                                 {{-1, -1, 1}, {1, -1, 1}, {-1, -1, -1}}, {{-1, 1, 1}, {1, 1, 1}, {-1, -1, 1}},   {{1, 1, -1}, {-1, 1, -1}, {1, -1, -1}}};
  out.assign(kCubeVertices, CubeVertex{});
  const double step = double(tile) / double(kCubeGrid - 1);
  for (uint32_t f = 0; f < kCubeFaces; ++f)
    for (uint32_t i = 0; i < kCubeGrid; ++i)
      for (uint32_t j = 0; j < kCubeGrid; ++j) {
        double d[3];
        for (int c = 0; c < 3; ++c) {
          const double a = kCorners[f][0][c], b = kCorners[f][1][c], e = kCorners[f][2][c];
          d[c] = a + (b - a) * (double(j) / 10.0) + (e - a) * (double(i) / 10.0);
        }
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        CubeVertex& v = out[(f * kCubeGrid + i) * kCubeGrid + j];
        v.pos[0] = float(double(tile) * f + step * j), v.pos[1] = float(-step * i), v.pos[2] = 0.0f;
        v.dir[0] = float(d[0] / len), v.dir[1] = float(d[1] / len), v.dir[2] = float(d[2] / len);
      }
}

// One strip per row pair of a face (the later row first), the face's vertices offset by its base, then the restart.
inline void BuildCubeIndices(std::vector<uint16_t>& out) {
  out.clear();
  for (uint32_t f = 0; f < kCubeFaces; ++f) {
    const uint32_t base = f * kCubeGrid * kCubeGrid;
    for (uint32_t k = 0; k + 1 < kCubeGrid; ++k) {
      for (uint32_t c = 0; c < kCubeGrid; ++c) out.push_back(uint16_t(base + (k + 1) * kCubeGrid + c)), out.push_back(uint16_t(base + k * kCubeGrid + c));
      out.push_back(kRestart);
    }
  }
}

// The largest absolute difference between two float arrays (NaN counts as infinite).
inline float MaxAbsDiff(const float* a, const float* b, size_t n) {
  float worst = 0;
  for (size_t i = 0; i < n; ++i) {
    const float d = std::fabs(a[i] - b[i]);
    if (!(d <= worst)) worst = d == d ? d : INFINITY;
  }
  return worst;
}

// The stream's index bytes as 16-bit values: the capture keeps the guest's big-endian order (reads of the geometry are raw); a host-order copy is accepted too. -1: neither order matches.
inline int CompareIndices(const uint8_t* bytes, size_t byte_count, const std::vector<uint16_t>& want) {
  if (byte_count != want.size() * 2) return -1;
  bool be = true, le = true;
  for (size_t i = 0; i < want.size() && (be || le); ++i) {
    const uint16_t b = uint16_t((bytes[2 * i] << 8) | bytes[2 * i + 1]), l = uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    be = be && b == want[i], le = le && l == want[i];
  }
  return be ? 1 : le ? 0 : -1;
}

}  // namespace rdr_scene::small_passes
