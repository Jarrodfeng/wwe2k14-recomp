// The plane shadow chain, the GPU-free core (Session 101, lane 139 = SH3 slice 2; docs/renderer/SHADOW_PLANE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game, SDK or renderer includes: the writer's tracker (src/scene_live/plane_hook.h), the DrawReplaceHook (plane_shadow_ours.cpp), fview's oracle and the unit test all run THIS code.
//
//  - BuildMask / BuildBlur: a PlaneFrame + PlaneDraw -> a RegTable, the vertex-constant rows of the stream draw that the record owns. The record's fields are named by what they are (rule 6); THIS file is
//    where they meet the 360's register numbers.
//  - Matches: exact equality of every owned row with the stream draw's own constants (the per-draw proof).
//  - Board: the record's frames and draws by guest-frame stamp, joined by (stamp, kind, object, part, nth) - never by the record's index; a record delivered twice adds nothing.
//  - Faults: the planted faults (plane_inv_identity, plane_camera_stale, plane_pos_shift, duplicate_record, late_record) and no_verify.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "scene_record.h"

namespace rdr_scene::plane {

inline constexpr uint32_t kMaskSite = 0x82BF4D38u;       // T_PlaneShadowMap: pass_site of the mask's part draws (shadow_roles::kPlanePassSite)
inline constexpr uint32_t kBlurCallSite = 0x827EEC84u;   // T_PlaneShadowBlur: call_site of the blur draws (yBlur plane_x, plane_z)
inline constexpr uint32_t kRunRows = 17;                 // the mask's override run: c0..c16
inline constexpr uint32_t kBlurFirstReg = 33, kBlurRunRows = 8;   // the blur's: c33..c40

struct Faults {
  bool inv_identity = false;      // R plane_inv_identity: the screen fit (c9-c12) is the identity instead of the fitted matrix: the mask is not fitted to the screen
  bool camera_stale = false;      // R plane_camera_stale: the camera (c13-c16) is the previous frame's: the mask lags the picture
  bool plane_pos_shift = false;   // R plane_pos_shift: the plane's position (c7) moved 40 units: the projected shadow slides (EyePos c72 is not owned, so it cannot be planted)
  bool duplicate_record = false;  // R duplicate_record: the board identifies a draw by (kind, object, part) and drops the later one: the second proxy instance no longer joins
  bool late_record = false;       // R late_record: the stream's frame is seen 6 frames ahead of the record (the record arrives late): every draw stays the stream's and the offset is searched again
  // lane 159 (the draws are ours now; each reaches the picture only with no_verify, otherwise the verify refuses the draw and it stays the stream's):
  bool blur_radius = false;       // R blur_radius: the blur's tap step is 8x the record's (the blur spreads wider than the stream's)
  bool nth_shift = false;         // R nth_shift: the record is read one entry off (the nth draw of a part takes the next instance's entry; the last one is unjoined)
  bool stamp_lock = false;        // R stamp_lock: the stamp -> frame offset is learned once, taken one frame too far, and never searched again
  bool blur_nan_lane = false;     // R blur_nan_lane (lane 205, check K3): TODAY's blur proof before the fix: all four lanes of c33 / c35 owned and compared by float == (a NaN stack leftover in y z w refuses the draw)
  bool join_double = false;       // R join_double (lane 255): the binder offers a casters-joined mask draw to this hook twice (claimed too many, the ordinals run ahead)
  bool join_drop = false;         // R join_drop (lane 255): the binder never asks this hook for every 7th casters-joined mask draw (claimed too few)
  bool no_verify = false;         // the safety check against the stream's own constants is skipped (so the planted faults reach the picture)
  static bool Parse(const std::string& list, Faults& f, std::string& error) {
    size_t at = 0;
    while (at <= list.size()) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      const std::string w = list.substr(at, comma - at);
      at = comma + 1;
      if (w.empty()) continue;
      if (w == "inv_identity" || w == "plane_inv_identity") f.inv_identity = true;
      else if (w == "camera_stale" || w == "plane_camera_stale") f.camera_stale = true;
      else if (w == "plane_pos_shift") f.plane_pos_shift = true;
      else if (w == "duplicate_record") f.duplicate_record = true;
      else if (w == "late_record") f.late_record = true;
      else if (w == "blur_radius") f.blur_radius = true;
      else if (w == "nth_shift") f.nth_shift = true;
      else if (w == "stamp_lock") f.stamp_lock = true;
      else if (w == "blur_nan_lane") f.blur_nan_lane = true;
      else if (w == "join_double") f.join_double = true;
      else if (w == "join_drop") f.join_drop = true;
      else if (w == "no_verify") f.no_verify = true;
      else {
        error = "unknown plane fault '" + w + "' (inv_identity, camera_stale, plane_pos_shift, duplicate_record, late_record, blur_radius, nth_shift, stamp_lock, blur_nan_lane, join_double, join_drop, no_verify)";
        return false;
      }
    }
    return true;
  }
};

// The rows of one stream draw that the record owns, indexed by register.
struct RegTable {
  float v[64 * 4] = {};
  bool have[64] = {};
  bool strict_float = false;   // the plant blur_nan_lane: compare by float == only (the pre-fix comparator)
  uint8_t lanes[64] = {};   // lane 205: which lanes (bit 0 = x ... bit 3 = w) of the row the game WROTE; the others are stack leftovers and are neither compared nor trusted
  // `lane_mask` = the lanes the game wrote (default all four). The lanes outside it hold whatever the record carries and are never read as inputs (see BuildBlur / Matches).
  void Set(uint32_t reg, const float* row4, uint32_t lane_mask = 0xF) {
    if (reg >= 64) return;
    std::memcpy(v + 4 * reg, row4, 16);
    have[reg] = true;
    lanes[reg] = uint8_t(lane_mask & 0xF);
  }
};

// `prev_camera` (may be null) is the previous stamp's camera for the camera_stale plant.
inline void BuildMask(const PlaneFrame* f, const PlaneDraw& d, const Faults& flt, const float* prev_camera, RegTable& t) {
  t = RegTable();
  if (f) {
    if (f->have & kPlaneHaveFade)
      for (uint32_t r = 0; r < 7; ++r) t.Set(r, f->fade + r);   // row r = fade[r..r+3]: the sliding window the game uploads
    if (f->have & kPlaneHaveNormal) t.Set(8, f->plane_normal);
    if (f->have & kPlaneHaveFit) {
      float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      for (uint32_t r = 0; r < 4; ++r) t.Set(9 + r, flt.inv_identity ? id + 4 * r : f->fit_matrix + 4 * r);
    }
    if (f->have & kPlaneHaveCamera)
      for (uint32_t r = 0; r < 4; ++r) t.Set(13 + r, flt.camera_stale && prev_camera ? prev_camera + 4 * r : f->camera + 4 * r);
  }
  if (d.have & kPlaneDrawHavePos) {
    float p[4] = {d.plane_pos[0], d.plane_pos[1], d.plane_pos[2], d.plane_pos[3]};
    if (flt.plane_pos_shift) p[1] += 40.f;
    t.Set(7, p);
  }
}
inline void BuildBlur(const PlaneDraw& d, RegTable& t, const Faults& flt = Faults()) {
  t = RegTable();
  if (!(d.have & kPlaneDrawHaveBlur)) return;
  float scale[4] = {d.blur_scale[0], d.blur_scale[1], d.blur_scale[2], d.blur_scale[3]};
  if (flt.blur_radius) scale[0] *= 8.0f;   // R blur_radius
  // Lane 205 (check K3): sub_827ECD10 stores ONE float into the stack slot r1+80 and uploads the whole 16-byte row from it: c33 is `lfs f0,96(r31); stfs f0,80(r1)` (0x827ECDA8-0x827ECDAC) then the
  // upload at 0x827ECDB0, c35 is `lfs f12,104(r31); stfs f12,80(r1)` (0x827ECDD0-0x827ECDD4) then 0x827ECDE8. Lanes y, z, w are whatever the stack held (0xad0e43b0 was seen in w): not the game's
  // value, so they are not compared (a leftover that is a NaN would fail a float == even with identical bytes) and our shader does not read them (BlurStep.x only; c35 is not read at all).
  const uint32_t one = flt.blur_nan_lane ? 0xFu : 0x1u;   // (the plant owns and compares all four lanes, by float ==: today's code)
  t.strict_float = flt.blur_nan_lane;
  t.Set(33, scale, one);
  t.Set(35, d.blur_weight, one);
  for (uint32_t r = 0; r < 4; ++r) t.Set(36 + r, d.blur_matrix + 4 * r);
  t.Set(40, d.blur_point);
}

// `row_of(reg)` returns the stream draw's 4 floats of that register, or null when the program has no such register (not verified: the program cannot read it).
// Exact equality (float ==, so -0 == 0): the record carries the very words the game's draw used. `compared` = the number of rows compared (0 = nothing to verify against).
// Lane 205: only the lanes the game wrote (RegTable::lanes) are compared, and by BITS first (a NaN in a written lane is equal when the bytes are), then by float == (-0 == 0, as before).
// `first_diff` (may be null) gets the first row that differs (register, lane mask, the stream's and the record's four words) for the log.
struct MatchDiff {
  uint32_t reg = 0, lane = 0;
  uint32_t stream[4] = {}, record[4] = {};
  bool set = false;
};
inline bool LaneEqual(float a, float b) {
  uint32_t x, y;
  std::memcpy(&x, &a, 4), std::memcpy(&y, &b, 4);
  return x == y || a == b;
}
template <class RowOf>
inline bool Matches(const RegTable& t, RowOf row_of, uint32_t* compared = nullptr, MatchDiff* first_diff = nullptr) {
  uint32_t n = 0;
  bool ok = true;
  for (uint32_t r = 0; r < 64; ++r) {
    if (!t.have[r]) continue;
    const float* s = row_of(r);
    if (!s) continue;
    ++n;
    for (int i = 0; i < 4; ++i) {
      if (!(t.lanes[r] & (1u << i))) continue;
      if (t.strict_float ? t.v[4 * r + i] == s[i] : LaneEqual(t.v[4 * r + i], s[i])) continue;
      ok = false;
      if (first_diff && !first_diff->set) {
        first_diff->set = true, first_diff->reg = r, first_diff->lane = uint32_t(i);
        std::memcpy(first_diff->stream, s, 16), std::memcpy(first_diff->record, t.v + 4 * r, 16);
      }
    }
  }
  if (compared) *compared = n;
  return ok;
}

// ---- the board: frames and draws by stamp ---------------------------------------------------------------------------------------------------------------------------------
class Board {
 public:
  explicit Board(bool dedupe_by_part = false) : by_part_(dedupe_by_part) {}
  // Adds a record's frames and draws. An entry already held (same stamp, kind, ordinal) is REPLACED, so a record delivered twice adds nothing; the plant duplicate_record identifies by
  // (kind, object, part) instead, which folds the second proxy instance of a part into the first.
  void Add(const std::vector<PlaneFrame>& frames, const std::vector<PlaneDraw>& draws) {
    for (const PlaneFrame& f : frames) frames_[f.frame] = f;
    for (const PlaneDraw& d : draws) {
      std::vector<PlaneDraw>& v = draws_[d.frame];
      bool found = false;
      for (PlaneDraw& have : v)
        if (have.kind == d.kind && (by_part_ ? (have.object == d.object && have.part == d.part) : have.ordinal == d.ordinal)) have = d, found = true;
      if (!found) v.push_back(d);
    }
    for (auto& kv : draws_)
      std::stable_sort(kv.second.begin(), kv.second.end(), [](const PlaneDraw& a, const PlaneDraw& b) { return a.ordinal < b.ordinal; });
    while (draws_.size() > kKeep) draws_.erase(draws_.begin());
    while (frames_.size() > kKeep) frames_.erase(frames_.begin());
  }
  // The nth draw of the stamp for the (kind, object, part); blur ignores object / part and takes the nth blur draw.
  const PlaneDraw* Find(uint32_t stamp, uint32_t kind, uint32_t object, uint32_t part, uint32_t nth) const {
    const auto it = draws_.find(stamp);
    if (it == draws_.end()) return nullptr;
    uint32_t seen = 0;
    for (const PlaneDraw& d : it->second) {
      if (d.kind != kind) continue;
      if (kind == kPlaneMask && (d.object != object || d.part != part)) continue;
      if (seen++ == nth) return &d;
    }
    return nullptr;
  }
  const PlaneFrame* FrameAt(uint32_t stamp) const {
    const auto it = frames_.find(stamp);
    return it == frames_.end() ? nullptr : &it->second;
  }
  // The previous stamp's camera (for the camera_stale plant): the newest frame block older than `stamp` with a camera, or null.
  const float* CameraBefore(uint32_t stamp) const {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
      if (it->first < stamp && (it->second.have & kPlaneHaveCamera)) return it->second.camera;
    return nullptr;
  }
  bool newest_stamp(uint32_t& out) const {
    if (draws_.empty()) return false;
    out = draws_.rbegin()->first;
    return true;
  }
  size_t stamps() const { return draws_.size(); }
  size_t draw_count() const {
    size_t n = 0;
    for (const auto& kv : draws_) n += kv.second.size();
    return n;
  }
  void Clear() { frames_.clear(), draws_.clear(); }

 private:
  static constexpr size_t kKeep = 48;
  bool by_part_ = false;
  std::map<uint32_t, PlaneFrame> frames_;
  std::map<uint32_t, std::vector<PlaneDraw>> draws_;
};

// The stamp -> stream frame offset: the hook's frame (VdSwap count when the game uploaded the constants) and the capture's header frame differ by an offset nobody has measured live [U].
// The join tries these in order until one entry verifies, remembers it, and tries again after kRelearn misses in a row (every kind of miss counts, as for the circles).
inline constexpr int kDeltaFirst = -2, kDeltaLast = 3, kRelearn = 8;

}  // namespace rdr_scene::plane
