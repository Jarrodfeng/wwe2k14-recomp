// S103 W2: the wrestlers' body scale, by the game's skeleton object address (the key of Object / SceneTables::engine_objects).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game / SDK / renderer includes.
//
// Why: the game's wrestler vertex is the file's position times ONE scale per body (0.8945 .. 1.0105; docs\formats\wrestler_host_vertex.md), and the bone palette
// it skins them with does NOT carry that scale (the palette rows are unit rotations; the bone lengths are the file's). A mesh drawn from the file's own positions
// through the record's palette is therefore off by (1 - s) times the vertex's bind-space coordinates, rotated per bone: 6 to 9 units on average and up to 21
// (a hand is a few units wide), so neighbouring bones pull the fingers and the face off each other (long curved strands, bulging eyes). S103 W2 measured it
// per node group (tools\wrestler\w2_skin_path_check.cpp: FE_NOSCALE vs FE_SCALED). The front-end fixes it by scaling the palette's linear rows by s
// (p * diag(s,s,s,1) * M), which is exactly "the vertex times s first".
//
// Who writes it: the wrestlers' core (wrestler_core.h) when it joins a stream object to a roster model and measures the scale from the stream's own vertices.
// Who reads it: rf_wrestlers.cpp when it builds the frame's palette. Unknown = 0 (the palette stays unscaled, counted). A scale is never guessed.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>

namespace rdr_scene {

class BodyScaleTable {
 public:
  static BodyScaleTable& Get() {
    static BodyScaleTable t;
    return t;
  }
  // A scale outside 0.1 .. 10 is not a body scale (the core refuses those too): ignored.
  void Set(uint32_t object_addr, float scale) {
    if (!object_addr || !(scale > 0.1f && scale < 10.0f)) return;
    std::lock_guard<std::mutex> g(mu_);
    map_[object_addr] = scale;
  }
  // By the record's object id ("actor:slot:0"): for a record that carries no engine object addresses (a pre-lane-B capture in the offline rig). The live path always has addresses.
  void SetByName(const std::string& id, float scale) {
    if (id.empty() || !(scale > 0.1f && scale < 10.0f)) return;
    std::lock_guard<std::mutex> g(mu_);
    by_name_[id] = scale;
  }
  float FindByName(const std::string& id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = by_name_.find(id);
    return it == by_name_.end() ? 0.f : it->second;
  }
  // 0 = not measured (yet).
  float Find(uint32_t object_addr) const {
    if (!object_addr) return 0.f;
    std::lock_guard<std::mutex> g(mu_);
    auto it = map_.find(object_addr);
    return it == map_.end() ? 0.f : it->second;
  }
  void Clear() {
    std::lock_guard<std::mutex> g(mu_);
    map_.clear();
    by_name_.clear();
    latch_.clear();
  }

  // CENSUS ONLY (never used for drawing): the float the game keeps in the participant record of the wrestler's character slot ([[0x83924734] + 864 + 80 x slot] + 12,
  // docs\systems\wrestler_pose.md "S103 W2 follow-up"), read at the scene latch, by the skeleton object address. A finite value is stored; `unreadable` counts the rest.
  void SetLatch(uint32_t object_addr, int slot, float v, bool readable) {
    std::lock_guard<std::mutex> g(mu_);
    if (!readable || !(v == v) || v > 1e6f || v < -1e6f) { ++latch_unreadable_; latch_.erase(object_addr); return; }   // S103 W3: an unreadable record leaves no stale value
    latch_[object_addr] = Latch{slot, v};
  }
  // S103 W3: the latch is now the SOURCE of the scale (BodyScaleFor). A latch entry is the float of the object's CURRENT slot record: an object that is not slot-matched this update
  // (an address the game reused) must not keep the old occupant's value (run 28, frame 12049: object 0xA89AA220 latch 0.894531 = the old occupant, stream 0.910400 = the new one).
  void ClearLatch(uint32_t object_addr) {
    std::lock_guard<std::mutex> g(mu_);
    latch_.erase(object_addr);
  }
  // 0 = no latch value for this object.
  float FindLatch(uint32_t object_addr) const {
    if (!object_addr) return 0.f;
    std::lock_guard<std::mutex> g(mu_);
    auto it = latch_.find(object_addr);
    return it == latch_.end() || !(it->second.value > 0.1f && it->second.value < 10.0f) ? 0.f : it->second.value;
  }
  struct CensusResult {
    uint32_t same = 0, differ = 0, unjoined = 0;   // unjoined = a latch value whose object the stream has not measured (no compare possible)
    uint64_t unreadable = 0;
    bool have_first = false;
    uint32_t first_addr = 0;
    int first_slot = -1;
    float first_latch = 0.f, first_stream = 0.f;
  };
  CensusResult Census(float tolerance = 1e-4f) const {
    std::lock_guard<std::mutex> g(mu_);
    CensusResult r;
    r.unreadable = latch_unreadable_;
    for (const auto& kv : latch_) {
      auto it = map_.find(kv.first);
      if (it == map_.end()) { ++r.unjoined; continue; }
      const float d = kv.second.value - it->second;
      if ((d < 0 ? -d : d) <= tolerance) ++r.same;
      else {
        ++r.differ;
        if (!r.have_first) r.have_first = true, r.first_addr = kv.first, r.first_slot = kv.second.slot, r.first_latch = kv.second.value, r.first_stream = it->second;
      }
    }
    return r;
  }
  // The census line: `body_scale latch==stream N, differ M, unjoined K` plus the first differing pair.
  static std::string CensusText(const CensusResult& r) {
    char b[320];
    int n = std::snprintf(b, sizeof b, "body_scale latch==stream %u, differ %u, unjoined %u (latch unreadable %llu)", r.same, r.differ, r.unjoined, (unsigned long long)r.unreadable);
    if (r.have_first) std::snprintf(b + n, sizeof b - size_t(n), "; first differing: object 0x%08X slot %d latch %.6f stream %.6f", r.first_addr, r.first_slot, double(r.first_latch), double(r.first_stream));
    return b;
  }

 private:
  mutable std::mutex mu_;
  struct Latch { int slot; float value; };
  std::unordered_map<uint32_t, float> map_;
  std::unordered_map<std::string, float> by_name_;
  std::unordered_map<uint32_t, Latch> latch_;
  uint64_t latch_unreadable_ = 0;
};

// The decision rf_wrestlers.cpp takes for one wrestler object: the scale its palette takes, or 0 = HOLE (never drawn unscaled; the live caller treats a hole as a miss, so the
// frame is not ours and the game draws it). A Viewer record (the viewer's own pose on its own unscaled mesh) needs no game scale: 1. `unknown_as_one` is the planted twin
// (the old behaviour: an unknown scale drawn unscaled); only the offline check sets it.
// S103 W3: the scale comes from the game's own record (the latch, read at the scene latch: [[0x83924734] + 864 + 80 x slot] + 12). Run 28 (mirror, entrances + match): latch == stream
// on every joined wrestler of 18 of 19 census windows (4-6 objects each, tolerance 1e-4); the stream's fit of the same object (tools\wrestler\w2_skin_path_check --scales) is the same k/4096
// value (0.894531248 = 3664/4096). The stream value is the mirror-mode compare: where both exist and DISAGREE the scale is unknown (a hole, never a guess; `conflict` says so).
// A stream value with no latch (the slot was not matched) is the old W2 behaviour. Neither = 0 = HOLE.
inline float BodyScaleFor(bool live_record, uint32_t object_addr, bool force_unknown = false, bool unknown_as_one = false, const std::string* object_id = nullptr, bool* conflict = nullptr) {
  if (!live_record) return 1.f;
  if (conflict) *conflict = false;
  float s = force_unknown ? 0.f : BodyScaleTable::Get().Find(object_addr);
  if (!(s > 0.f) && !force_unknown && object_id) s = BodyScaleTable::Get().FindByName(*object_id);   // (the offline rig measures by record object id: its records need not carry the engine addresses the stream draws name)
  const float l = force_unknown ? 0.f : BodyScaleTable::Get().FindLatch(object_addr);
  if (l > 0.f && s > 0.f) {
    const float d = l - s;
    if ((d < 0 ? -d : d) > 1e-4f) { if (conflict) *conflict = true; return unknown_as_one ? 1.f : 0.f; }
    return l;
  }
  if (l > 0.f) return l;
  return s > 0.f ? s : (unknown_as_one ? 1.f : 0.f);
}
// Rows 0-2 of a row-vector matrix times s (p * diag(s,s,s,1) * M): the palette matrix that skins a vertex the game would have scaled by s first.
inline void ScalePaletteRows(float* m16, float s) {
  for (int k = 0; k < 12; ++k) m16[k] *= s;
}

}  // namespace rdr_scene
