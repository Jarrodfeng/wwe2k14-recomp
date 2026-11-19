// The ring pieces skinned the way the GPU does it (S103 lane RGP; docs\research\s103\RGS.md option 1): the CPU-side twin of the front end's skinned-prop draw, for the offline proof only (tools\scene\ring_pose_unit.cpp,
// the dev verify line of rf_ring.cpp). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WHAT THE GAME-SIDE PATH DOES [C]: a skinned mesh is cut into palette chunks (char_effects.cpp BuildSkinChunks: <= 21 bones, a skin stream of palette SLOT ids + weights, `nodes` = slot -> skeleton node); the draw feeds
// WorldMatrixArray slot s = skin[nodes[s]] x object world (front_end.cpp Record: a node past the palette = identity) as 3 rows a slot (row r = column r of the row-vector matrix, front_end.cpp Resolve) and Bools bit 3 (four
// bones); the vertex stage is fx_prop.glsl prop_skin -> fx_character.glsl ch_skin with boneCount 4:  pos = p0 w.x + p1 w.y + p2 w.z + p3 (1 - w.x - w.y - w.z), p_k = (dot(row0, P), dot(row1, P), dot(row2, P)) of slot ids.k,
// a slot id past the block's 21 slots = zero (ch_bone). This header repeats that arithmetic in the shader's order, single precision, so a unit can compare it with PoseMesh (ring_pose.h).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "char_effects.h"
#include "ring_pose.h"

namespace rdr_scene::rf::c {

// Slot matrices of one chunk as the front end builds them (palette = the call's viewer-space matrices, world = identity for a ring piece). `plant_slot_off` = planted fault: slot s takes the node of slot s + 1 (a wrong slot -> node map).
inline std::vector<assets::Matrix> ChunkSlots(const SkinChunk& c, const assets::Matrix* pal, size_t count, bool plant_slot_off = false) {
  std::vector<assets::Matrix> slots;
  slots.reserve(c.nodes.size());
  const assets::Matrix world = assets::Identity();
  for (size_t s = 0; s < c.nodes.size(); ++s) {
    const uint16_t node = c.nodes[plant_slot_off ? (s + 1) % c.nodes.size() : s];
    assets::Matrix sk = assets::Identity();
    if (node < count) sk = pal[node];
    slots.push_back(assets::Mul(sk, world));
  }
  return slots;
}

// fx_character.glsl ch_bone_point on the WorldMatrixArray rows the front end writes (row r of slot s = column r of the slot's matrix).
inline void SlotPoint(const std::vector<assets::Matrix>& slots, uint32_t id, const float p[3], float out[3]) {
  if (id >= 21 || id >= slots.size()) { out[0] = out[1] = out[2] = 0.f; return; }   // ch_bone: past the 63-vec4 block (or no such slot) reads as zero
  const assets::Matrix& m = slots[id];
  for (int r = 0; r < 3; ++r) out[r] = m[size_t(r)] * p[0] + m[size_t(4 + r)] * p[1] + m[size_t(8 + r)] * p[2] + m[size_t(12 + r)] * 1.f;
}

// prop_skin, Bools bit 3 (four bones): the vertex stage's world position of one skinned vertex.
inline void ShaderSkinPoint(const float p[3], const SkinVertex& sv, const std::vector<assets::Matrix>& slots, float out[3]) {
  float p0[3], p1[3], p2[3], p3[3];
  SlotPoint(slots, sv.ids[0], p, p0);
  SlotPoint(slots, sv.ids[1], p, p1);
  SlotPoint(slots, sv.ids[2], p, p2);
  SlotPoint(slots, sv.ids[3], p, p3);
  const float wx = sv.w[0], wy = sv.w[1], wz = sv.w[2];
  const float rest = 1.f - wx - wy;
  for (int c = 0; c < 3; ++c) out[c] = p0[c] * wx + p1[c] * wy + p2[c] * wz + p3[c] * (rest - wz);
}

// The whole bound mesh through the chunk path: out[3 * v .. 3 * v + 2] = the GPU position of mesh vertex v. false = BuildSkinChunks refused (a triangle needs more than 21 bones).
// done[v] = 1 when some triangle draws vertex v (a vertex no index names is never drawn: out holds 0 there and the caller skips it).
inline bool GpuSkinMesh(const assets::Mesh& bound, const assets::Matrix* pal, size_t count, std::vector<float>& out, std::vector<char>& done, bool plant_slot_off = false) {
  std::vector<SkinChunk> chunks;
  if (!BuildSkinChunks(bound, chunks)) return false;
  out.assign(bound.vertices.size() * 3, 0.f);
  done.assign(bound.vertices.size(), 0);
  for (const SkinChunk& c : chunks) {
    const std::vector<assets::Matrix> slots = ChunkSlots(c, pal, count, plant_slot_off);
    for (size_t i = 0; i < c.vertices.size(); ++i) {
      const uint32_t src = c.src[i];
      float o[3];
      ShaderSkinPoint(c.vertices[i].pos, c.skin[i], slots, o);
      for (int k = 0; k < 3; ++k) out[3 * size_t(src) + size_t(k)] = o[k];
      done[src] = 1;
    }
  }
  return true;
}

// max |p_gpu - p_cpu| over the drawn vertices of one BOUND mesh (BindMesh done): the CPU pose = PoseMesh (ring_pose.h, what the posed ring draws today), the GPU one = GpuSkinMesh. -1 = the chunk cut failed.
inline double MaxPoseError(const assets::Mesh& bound, const assets::Matrix* pal, size_t count, bool plant_slot_off = false) {
  assets::Mesh cpu = bound;
  PoseMesh(bound, pal, count, cpu);
  std::vector<float> gpu;
  std::vector<char> done;
  if (!GpuSkinMesh(bound, pal, count, gpu, done, plant_slot_off)) return -1.0;
  double worst = 0.0;
  for (size_t v = 0; v < bound.vertices.size(); ++v) {
    if (!done[v]) continue;
    for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(double(gpu[3 * v + size_t(k)]) - double(cpu.vertices[v].pos[k])));
  }
  return worst;
}

}  // namespace rdr_scene::rf::c
