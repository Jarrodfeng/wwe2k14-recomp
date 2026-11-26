// The ring pieces posed on the CPU (S103 lane C4; docs\renderer\STAGE_LIVE_WRITERS.md section 3c). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game, SDK or renderer includes: the applier (rf_ring.cpp) and the offline unit (tools\scene\ring_pose_unit.cpp) run THESE functions.
//
// WHAT THE GAME DOES [C listing sub_82C04DF0 0x82C0A00C-0x82C0A098]: for the part descriptor r21 = [model + 824] + 184 * part (the file's own mesh record: count at +8, 1-based node numbers at +12 + 4 k), slot k of the
// palette is the matrix at r5 + 64 * (v - 1) (v = the word at r21 + 12 + 4 k): the call's palette array IS the rows (RingRows pool order), and the slot -> row map of a part is `palette[k] - 1`. The asset loader keeps that
// map per mesh (jboy.h: Mesh::palette_nodes, rigid_node) and the file's raw skin pairs (src_skin_slots / src_skin_weights), because an ARENA model has no merged skeleton (its vertex.bones are all 0).
// WHAT THIS DOES: BindMesh rebuilds each vertex's (node, weight) pairs from those; PoseMesh skins the stored mesh (viewer space, the loader's Y mirror) with the call's rows made viewer-space matrices
// (WorldFromRows = S M S): v' = sum_k w_k (v M_k) / sum w, normals and tangents with the rotation part. A vertex with no influence in the palette keeps its stored position. Single precision.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../assets/asset_model.h"

namespace rdr_scene::rf::c {

// The ONE node row -> matrix function (S103 PW review 9: the weapon drawer had its own copy, ViewerFromRows): VS c76..c78 (row k = rows[4k .. 4k+3], the 4th column = the translation) as a row-vector matrix in the viewer's
// space (S X S). rf::c::WorldFromRows (rf_c_core.cpp) is this with the ring's translation-only twin; the weapon drawer calls this directly (header only: the offline units need no rf_c_core.cpp).
inline assets::Matrix WorldFromRowsCore(const float rows[12], bool translation_only = false) {
  assets::Matrix m = assets::Identity();
  for (int k = 0; k < 3; ++k)
    for (int r = 0; r < 4; ++r) m[size_t(4 * r + k)] = rows[4 * k + r];  // live::arenaworld::MatrixFromRows
  m[3] = m[7] = m[11] = 0.f, m[15] = 1.f;
  if (translation_only)
    for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) m[size_t(i)] = (i % 5 == 0) ? 1.f : 0.f;
  return assets::MirrorY(m);  // the game's file space -> the viewer's: S X S
}

// Binds a stored ring mesh to the SKELETON NODES the game uses (the pool index of the call's rows): a mesh with skin pairs takes, per vertex, node = palette_nodes[slot] with the file's own weights
// (src_skin_slots / src_skin_weights, 4 pairs a vertex, slot 255 = none; normalised); a mesh with none (the covers' alternative, rigid parts) is bound to rigid_node on every vertex. The loader's
// vertex.bones of an arena mesh are all 0 (no merged skeleton), so this REPLACES them. false = the mesh lacks what the bind needs (it stays unposed: the caller draws the key at rest).
inline bool BindMesh(assets::Mesh& m) {
  const size_t n = m.vertices.size();
  if (m.src_skin_pairs != 0) {
    if (m.src_skin_slots.size() < n * 4 || m.src_skin_weights.size() < n * 4 || m.palette_nodes.empty()) return false;
    for (size_t i = 0; i < n; ++i) {
      assets::Vertex& v = m.vertices[i];
      float sum = 0.f;
      int nb = 0;
      for (int k = 0; k < 4; ++k) v.bones[k] = 0, v.weights[k] = 0.f;
      for (int k = 0; k < 4; ++k) {
        const uint8_t slot = m.src_skin_slots[i * 4 + size_t(k)];
        const float w = m.src_skin_weights[i * 4 + size_t(k)];
        if (slot == 255 || size_t(slot) >= m.palette_nodes.size() || m.palette_nodes[slot] == 0xFFFF || !(w > 0.f)) continue;
        v.bones[nb] = m.palette_nodes[slot], v.weights[nb] = w, sum += w, ++nb;
      }
      if (nb == 0) {   // no usable influence: stay on the palette's first node (the loader's own fallback, jboy.h)
        v.bones[0] = m.palette_nodes[0] == 0xFFFF ? 0 : m.palette_nodes[0], v.weights[0] = 1.f;
      } else if (sum > 0.f && std::fabs(sum - 1.f) > 1e-4f) {
        for (int k = 0; k < nb; ++k) v.weights[k] /= sum;
      }
    }
    return true;
  }
  if (m.rigid_node < 0 || m.rigid_node > 65535) return false;
  for (assets::Vertex& v : m.vertices) {
    v.bones[0] = uint16_t(m.rigid_node), v.bones[1] = v.bones[2] = v.bones[3] = 0;
    v.weights[0] = 1.f, v.weights[1] = v.weights[2] = v.weights[3] = 0.f;
  }
  return true;
}

// Skins `stored` into `out` (same vertex count; the indices and everything else of `out` are left as they are). `pal` = `count` viewer-space matrices (16 floats each, row vectors, translation at [12..14]).
inline void PoseMesh(const assets::Mesh& stored, const assets::Matrix* pal, size_t count, assets::Mesh& out) {
  if (out.vertices.size() != stored.vertices.size()) out.vertices = stored.vertices;
  for (size_t i = 0; i < stored.vertices.size(); ++i) {
    const assets::Vertex& s = stored.vertices[i];
    assets::Vertex& d = out.vertices[i];
    d = s;
    float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[3] = {0, 0, 0}, wsum = 0;
    for (int k = 0; k < 4; ++k) {
      const float w = s.weights[k];
      if (!(w > 0.f) || size_t(s.bones[k]) >= count) continue;
      const assets::Matrix& M = pal[s.bones[k]];
      for (int c = 0; c < 3; ++c) {
        p[c] += w * (s.pos[0] * M[size_t(c)] + s.pos[1] * M[size_t(4 + c)] + s.pos[2] * M[size_t(8 + c)] + M[size_t(12 + c)]);
        n[c] += w * (s.normal[0] * M[size_t(c)] + s.normal[1] * M[size_t(4 + c)] + s.normal[2] * M[size_t(8 + c)]);
        t[c] += w * (s.tangent[0] * M[size_t(c)] + s.tangent[1] * M[size_t(4 + c)] + s.tangent[2] * M[size_t(8 + c)]);
      }
      wsum += w;
    }
    if (!(wsum > 0.f)) continue;
    const float inv = 1.f / wsum;
    for (int c = 0; c < 3; ++c) d.pos[c] = p[c] * inv, d.normal[c] = n[c] * inv, d.tangent[c] = t[c] * inv;
  }
}

// S108 FAN / AL1 (moved here from weapon_draw_core.h: the ring's pieces need it too): the rows a BOUND model's vertices ask for = the highest node an influence with weight > 0 names, + 1 (0 = none).
// PoseMesh above skips exactly the influences with `bones[k] >= count` and `w > 0`, so `RowsNeeded(bound) > count` is exactly "some vertex is left (or partly left) at its file position". Computed ONCE per bound
// model (it walks every vertex); the callers compare it with each call's rows (pose_rows_report.h).
inline uint32_t RowsNeeded(const assets::Model& bound) {
  uint32_t need = 0;
  for (const assets::Mesh& m : bound.meshes)
    for (const assets::Vertex& v : m.vertices)
      for (int k = 0; k < 4; ++k)
        if (v.weights[k] > 0.f && uint32_t(v.bones[k]) + 1u > need) need = uint32_t(v.bones[k]) + 1u;
  return need;
}

// ---- S103 RGP: the call's palette (the GPU-skinned ring's bones, rf_ring.cpp; the unit runs this function) ------------------------------------------------------------------------------------------------
// The call's n_nodes rows starting at `first_node` of the record's node pool, each made a viewer-space matrix (pool index k = node k, as BindMesh's nodes and the CPU pose read them). `no_pose` = the ring_no_pose plant (every node
// the identity). Node = any type with a float rows[12] member (RingNodeRows; the unit's own struct).
template <class Node>
inline void RingPalette(const Node* nodes, uint32_t first_node, uint32_t n_nodes, bool no_pose, bool translation_only, std::vector<assets::Matrix>& out) {
  out.resize(n_nodes);
  for (uint32_t k = 0; k < n_nodes; ++k) out[k] = no_pose ? assets::MirrorY(assets::Identity()) : WorldFromRowsCore(nodes[first_node + k].rows, translation_only);
}

// ---- S103 RGP: which bound ring keys the GPU skin draws (docs\research\s103\RGS.md option 1) ------------------------------------------------------------------------------------------------------------
// A key is drawn GPU-skinned only when EVERY mesh has a material whose effect is a yProp_* (the front end cuts a skinned mesh into palette chunks for the character effects and the yProp_* ones only: front_end.cpp
// HasSkinnedProgram; any other effect would stay rigid at rest and the piece would not move) and none of them is alpha-blended (a blended mesh sorts by its rest centre: UpdateModelVertices used to recompute it from the
// posed vertices). A key that fails stays on the CPU pose. false is also the answer for a mesh with no material.
inline bool GpuSkinnable(const assets::Model& m) {
  if (m.meshes.empty()) return false;
  for (const assets::Mesh& me : m.meshes) {
    if (me.material < 0 || size_t(me.material) >= m.materials.size()) return false;
    const assets::Material& mt = m.materials[size_t(me.material)];
    if (mt.shader.rfind("yProp_", 0) != 0 || mt.alpha_blend) return false;
  }
  return true;
}

// ---- S103 C5: the posed pools' names, and the skip of an unchanged pose --------------------------------------------------------------------------------------------------------------------------------
// A posed pool is named by the ARENA SET's key and the key the entry ASKED for (not the one Resolve fell back to: a 980 -> 960 rope and a high rope 960 must not share a slot per call index). Every pool of a set
// starts with PoolPrefix(set key), so ModelCache::RetireOther(PoolPrefix(old key), "\x01") drops all of them when the set is replaced.
inline std::string PoolPrefix(const std::string& set_key) { return set_key + ":ringpose:"; }
inline std::string PoolName(const std::string& set_key, int asked_key) { return PoolPrefix(set_key) + std::to_string(asked_key); }
// S103 RGP review F2: the kept draw lists' instance key (Object::pad, a u16) of a GPU-skinned ring instance. `idx` is the call's index among the calls that ASKED for this model address; Resolve may map a missing low
// key (980 / 981 / 982) to its high one, and then high call k and low call k resolve to ONE model with the same idx: the ASKED-vs-RESOLVED bit keeps them distinct by construction (pad = 1 + 2 * idx + fell_back).
// Bound: idx < 32767 (the rows hook's per-key call count is a few hundred at most; the caller clamps it).
inline uint16_t RingInstancePad(uint32_t idx, bool fell_back) { return uint16_t(1u + 2u * (idx > 32766u ? 32766u : idx) + (fell_back ? 1u : 0u)); }
// The hash of an instance's rows (the pose is a pure function of them and of the stored mesh): word-wise FNV-style mix.
inline uint64_t HashRows(const float* rows, size_t floats, uint64_t seed) {
  uint64_t h = 1469598103934665603ull ^ seed;
  for (size_t i = 0; i < floats; ++i) {
    uint32_t w;
    std::memcpy(&w, &rows[i], 4);
    h = (h ^ w) * 1099511628211ull;
    h ^= h >> 29;
  }
  return h;
}
// true = pose and upload; false = the slot already holds exactly this pose (same rows hash, the slot is live). Records the hash when it answers true.
inline bool PoseNeeded(std::map<std::string, uint64_t>& memo, const std::string& skey, uint64_t hash, bool slot_live) {
  const auto it = memo.find(skey);
  if (slot_live && it != memo.end() && it->second == hash) return false;
  memo[skey] = hash;
  return true;
}

}  // namespace rdr_scene::rf::c
