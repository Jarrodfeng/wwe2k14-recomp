// The circle blob shadows, the GPU-free core (Session 101, lane 136 = SH3 slice 1; docs/renderer/SHADOW_CIRCLES.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game, SDK or renderer includes: the writer's tracker (src/scene_live/circle_hook.h), the DrawReplaceHook (circle_shadows_ours.cpp), fview's oracle and the unit test
// all run THIS code.
//
//  - BuildRows: a CircleShadow -> the vertex constants of the yCircleShadow program, c0 ShadowParam, c4-c7 ViewProj, c8-c28 WorldPosArray (29 rows; c1-c3 are not used by the program).
//  - Mesh conversion: the game's vertex buffer (eight big-endian 32-bit words a vertex, the point ids included) <-> the host layout the stream's draw uses.
//  - Board: the record's entries by their guest-frame stamp (never by an index into the record sequence).
//  - Faults: the planted faults (circle_bind_pose, circle_join_by_index, circle_no_fade) and no_verify.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "scene_record.h"

namespace rdr_scene::circles {

inline constexpr uint32_t kRows = 29;   // c0..c28
inline constexpr uint32_t kFirstRegister = 0;

struct Faults {
  bool bind_pose = false;        // R circle_bind_pose: every character's points are its FIRST pose seen (a static pose) instead of the frame's: the blobs stop following the body
  bool join_by_index = false;    // R circle_join_by_index: the entries are taken by the order records arrived (the previous record's), not by their guest-frame stamp
  bool no_fade = false;          // R circle_no_fade: the fade power is dropped (w = 0): a body point high above the floor casts a full-strength blob
  bool index_corrupt = false;    // R (review F2): one index of the uploaded mesh is wrong: the index-buffer check against the stream's must refuse the draw
  bool mesh_unreadable = false;  // R (review F2): the stream's geometry cannot be read: the draw must stay the stream's (and the check retried), never taken on the record's word
  bool late_record = false;      // R (review F3): from the 40th record on, each record reaches the hook one record late (a live writer behind the stream): the right stamp is missing, counted `late`, the stream's draw stays (nothing can recover a record that is not there)
  bool offset_shift = false;     // from the 40th record on every entry's stamp is one higher (the stamp -> frame offset moves mid-run): the relearn must find the new offset within 9 draws
  bool static_lock = false;      // R (review F3, offset_static_lock): the learned offset is never searched again, so a changed offset is never found
  bool float_compare = false;    // R float_compare (lane 215): the constants are compared by float == only (the pre-fix rule: a NaN never equals itself, so a bit-equal NaN ViewProj refuses the draw)
  bool no_verify = false;        // the safety check against the stream's own constants is skipped (so the planted faults reach the picture)
  static bool Parse(const std::string& list, Faults& f, std::string& error) {
    size_t at = 0;
    while (at <= list.size()) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      const std::string w = list.substr(at, comma - at);
      at = comma + 1;
      if (w.empty()) continue;
      if (w == "bind_pose" || w == "circle_bind_pose") f.bind_pose = true;
      else if (w == "join_by_index" || w == "circle_join_by_index") f.join_by_index = true;
      else if (w == "no_fade" || w == "circle_no_fade") f.no_fade = true;
      else if (w == "index_corrupt") f.index_corrupt = true;
      else if (w == "mesh_unreadable") f.mesh_unreadable = true;
      else if (w == "late_record") f.late_record = true;
      else if (w == "offset_shift") f.offset_shift = true;
      else if (w == "offset_static_lock" || w == "static_lock") f.static_lock = true;
      else if (w == "float_compare") f.float_compare = true;
      else if (w == "no_verify") f.no_verify = true;
      else {
        error = "unknown circles fault '" + w + "' (bind_pose, join_by_index, no_fade, index_corrupt, mesh_unreadable, late_record, offset_shift, offset_static_lock, float_compare, no_verify)";
        return false;
      }
    }
    return true;
  }
};

// The 29 rows of one entry. `first_pose` (may be null) is the bind_pose plant's static points. Rows c1-c3 are zero.
inline void BuildRows(const CircleShadow& e, const Faults& f, const CircleShadow* first_pose, float out[kRows * 4]) {
  std::memset(out, 0, sizeof(float) * kRows * 4);
  std::memcpy(out, e.shadow_param, sizeof e.shadow_param);
  if (f.no_fade) out[3] = 0.0f;
  std::memcpy(out + 16, e.view_proj, sizeof e.view_proj);
  const CircleShadow& p = (f.bind_pose && first_pose) ? *first_pose : e;
  std::memcpy(out + 32, p.points, sizeof p.points);
}

// The stream draw's own constants for the same rows, read from a packed vertex block at the program's three offsets (ShadowParam, ViewProj, WorldPosArray).
struct StreamRows {
  float c0[4] = {}, vp[16] = {}, pts[kCirclePoints * 4] = {};
};
inline bool ReadStreamRows(const uint8_t* block, uint32_t bytes, uint32_t off_c0, uint32_t off_vp, uint32_t off_pts, StreamRows& out) {
  if (!block || uint64_t(off_c0) + 16 > bytes || uint64_t(off_vp) + 64 > bytes || uint64_t(off_pts) + 16u * kCirclePoints > bytes) return false;
  std::memcpy(out.c0, block + off_c0, 16);
  std::memcpy(out.vp, block + off_vp, 64);
  std::memcpy(out.pts, block + off_pts, 16u * kCirclePoints);
  return true;
}
// Exact equality of every used constant: the record carries the very words the game's draw used. Lane 215 (checks 7132 / 7133, the victory scene): a word is equal when its BITS are equal OR it is
// float-equal (-0 == 0, as before). The bits come first because a NaN is never float-equal to itself, and the game's light matrix IS a NaN on some frames (ViewProj c4-c7, 0x7FC00000 in all 16 words,
// identical in the record and the stream draw): the old float == refused those draws as "constants" though the record held the very words. `strict_float` = the plant `float_compare` (the old rule).
struct RowDiff {   // the first word that differs (log first: which register, which lane, what each side held)
  bool found = false;
  uint32_t reg = 0, lane = 0, stream_bits = 0, record_bits = 0;
};
inline bool WordsEqual(float record, float stream, bool strict_float) {
  uint32_t x, y;
  std::memcpy(&x, &record, 4), std::memcpy(&y, &stream, 4);
  return (!strict_float && x == y) || record == stream;
}
inline bool RowsMatch(const float rows[kRows * 4], const StreamRows& s, bool strict_float = false, RowDiff* diff = nullptr) {
  const auto bad = [&](uint32_t reg, uint32_t lane, float record, float stream) {
    if (diff && !diff->found) {
      diff->found = true, diff->reg = reg, diff->lane = lane;
      std::memcpy(&diff->record_bits, &record, 4), std::memcpy(&diff->stream_bits, &stream, 4);
    }
    return false;
  };
  for (int i = 0; i < 4; ++i)
    if (!WordsEqual(rows[i], s.c0[i], strict_float)) return bad(0, uint32_t(i), rows[i], s.c0[i]);
  for (int i = 0; i < 16; ++i)
    if (!WordsEqual(rows[16 + i], s.vp[i], strict_float)) return bad(4 + uint32_t(i) / 4, uint32_t(i) % 4, rows[16 + i], s.vp[i]);
  for (uint32_t i = 0; i < kCirclePoints * 4; ++i)
    if (!WordsEqual(rows[32 + i], s.pts[i], strict_float)) return bad(8 + i / 4, i % 4, rows[32 + i], s.pts[i]);
  return true;
}

// ---- the disc mesh ------------------------------------------------------------------------------------------------------------------------------------------------------
// The game's vertex buffer: 32-byte vertices of eight 32-bit BIG-ENDIAN words (guest memory). The stream's host layout holds the same eight words as little-endian values, so EVERY word, the u8x4 point-id element
// (word 4, fmt 41 at offset 16) included, is byte-reversed: the fetch constant's endian field swaps whole 32-bit words and does not know about the element types. Session 101 lane 205 (check K3, run 7094): the first
// version kept word 4 "in memory order" and every circle mesh check refused (0 of 544 live circle draws equal, 544 of 544 with the word swapped too). Written word by word, from the byte order, not as a shared swap
// helper, so the conversion and its inverse (the oracle's fake guest) are two readings of the format rather than one function used twice.
inline uint32_t Swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
// keep_id_word = the old rule (test plant `id_word_kept`): word 4 left in memory order.
inline void VertexGuestToHost(const uint8_t* g, uint8_t* h, bool keep_id_word = false) {
  for (int w = 0; w < 8; ++w) {
    const uint8_t* s = g + 4 * w;   // guest: the most significant byte first
    const uint32_t value = (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | uint32_t(s[3]);
    uint8_t* d = h + 4 * w;         // host: the least significant byte first
    if (keep_id_word && w == 4) {
      std::memcpy(d, s, 4);
      continue;
    }
    d[0] = uint8_t(value), d[1] = uint8_t(value >> 8), d[2] = uint8_t(value >> 16), d[3] = uint8_t(value >> 24);
  }
}
// The inverse, from the format: a host word is a little-endian value, the guest stores it big-endian. (Used by the oracle's fake guest only.)
inline void VertexHostToGuest(const uint8_t* h, uint8_t* g) {
  for (int w = 0; w < 8; ++w) {
    const uint8_t* s = h + 4 * w;
    const uint32_t value = uint32_t(s[0]) | (uint32_t(s[1]) << 8) | (uint32_t(s[2]) << 16) | (uint32_t(s[3]) << 24);
    uint8_t* d = g + 4 * w;
    d[0] = uint8_t(value >> 24), d[1] = uint8_t(value >> 16), d[2] = uint8_t(value >> 8), d[3] = uint8_t(value);
  }
}
// False when the counts do not fit the fixed capacity or an index points past the vertices.
inline bool MeshFromGuest(const uint8_t* vertices, uint32_t vcount, const uint8_t* indices_be, uint32_t icount, CircleMesh& out, bool keep_id_word = false) {
  if (!vertices || !indices_be || !vcount || !icount || vcount > kCircleMeshVertices || icount > kCircleMeshIndices || icount % 3) return false;
  out = CircleMesh();
  out.vertex_count = vcount, out.index_count = icount;
  for (uint32_t v = 0; v < vcount; ++v) VertexGuestToHost(vertices + 32 * v, out.vertices + 32 * v, keep_id_word);
  for (uint32_t i = 0; i < icount; ++i) {
    out.indices[i] = uint16_t((uint32_t(indices_be[2 * i]) << 8) | indices_be[2 * i + 1]);
    if (out.indices[i] >= vcount) return false;
  }
  return true;
}
inline void MeshToGuest(const CircleMesh& m, std::vector<uint8_t>& vertices, std::vector<uint8_t>& indices_be) {
  vertices.assign(size_t(m.vertex_count) * 32, 0);
  indices_be.assign(size_t(m.index_count) * 2, 0);
  for (uint32_t v = 0; v < m.vertex_count; ++v) VertexHostToGuest(m.vertices + 32 * v, vertices.data() + 32 * v);
  for (uint32_t i = 0; i < m.index_count; ++i) indices_be[2 * i] = uint8_t(m.indices[i] >> 8), indices_be[2 * i + 1] = uint8_t(m.indices[i] & 0xFF);
}

// ---- the board: entries by stamp ----------------------------------------------------------------------------------------------------------------------------------------
class Board {
 public:
  // Adds a record's entries. An entry already held (same stamp, slot, character) is replaced, so a record delivered twice adds nothing.
  void Add(const std::vector<CircleShadow>& entries) {
    uint32_t newest_in_batch = 0;
    bool any = false;
    for (const CircleShadow& e : entries) {
      std::vector<CircleShadow>& v = by_stamp_[e.frame];
      bool found = false;
      for (CircleShadow& have : v)
        if (have.slot == e.slot && have.character == e.character) have = e, found = true;
      if (!found) v.push_back(e);
      newest_in_batch = any ? (e.frame > newest_in_batch ? e.frame : newest_in_batch) : e.frame, any = true;
    }
    if (any && (batches_ == 0 || newest_in_batch != last_batch_stamp_)) {   // (a record delivered twice is one batch)
      prev_batch_stamp_ = last_batch_stamp_, last_batch_stamp_ = newest_in_batch, ++batches_;
    }
    while (by_stamp_.size() > kKeep) by_stamp_.erase(by_stamp_.begin());
  }
  void SetMesh(const CircleMesh& m) { mesh_ = m, has_mesh_ = true; }
  bool has_mesh() const { return has_mesh_; }
  const CircleMesh& mesh() const { return mesh_; }
  // The entries stamped `stamp` in the order the game uploaded them (slot order), or null.
  const std::vector<CircleShadow>* At(uint32_t stamp) const {
    const auto it = by_stamp_.find(stamp);
    return it == by_stamp_.end() ? nullptr : &it->second;
  }
  // The join_by_index plant: the entries of the record BEFORE the newest (the crowd's j-1 pairing applied to a stamped section).
  const std::vector<CircleShadow>* PreviousBatch() const { return batches_ >= 2 ? At(prev_batch_stamp_) : nullptr; }
  size_t stamps() const { return by_stamp_.size(); }
  bool newest_stamp(uint32_t& out) const {   // the highest stamp held
    if (by_stamp_.empty()) return false;
    out = by_stamp_.rbegin()->first;
    return true;
  }
  void Clear() { by_stamp_.clear(), has_mesh_ = false, batches_ = 0; }

 private:
  static constexpr size_t kKeep = 48;
  std::map<uint32_t, std::vector<CircleShadow>> by_stamp_;
  CircleMesh mesh_;
  bool has_mesh_ = false;
  uint32_t last_batch_stamp_ = 0, prev_batch_stamp_ = 0;
  uint64_t batches_ = 0;
};

// ---- the stamp -> stream frame offset ------------------------------------------------------------------------------------------------------------------------------------
// The hook's frame (VdSwap count when the game uploaded the constants) and the capture's header frame differ by an offset nobody has measured live [U]. The join tries the offsets
// below in order until one entry verifies against the stream's own constants, remembers it, and tries again only after kRelearn misses in a row.
inline constexpr int kDeltaFirst = -2, kDeltaLast = 3, kRelearn = 8;

}  // namespace rdr_scene::circles
