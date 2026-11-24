// S103 S2: the CHANGES census per scene input (docs/renderer/S103_FABLE_REVIEW_2.md Q1 1.3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure, header-only: our record tables and the scene switch only (no game, SDK or renderer). live_scene.cpp calls Note on every built record; tools\scene\s2_unit.cpp drives it on made-up tables.
//
// What it answers, with no knowledge of any input's writer: for each required input bit, how often the record section(s) that carry its VALUE changed from the previous record, counted
// apart on OURS records (the walks of that hook frame were skipped) and on walked records (and, inside those, on sentinel walks: scene_frame.h IsSentinel). A healthy update-side input
// changes at the same rate on both. An input whose writer lives inside a skipped walk changes only on walked frames: on OURS frames it repeats ("fed by the walk"). A sentinel `jump` is
// the sharpest sign: the value stood still over the OURS frames before a sentinel and changed on the sentinel walk itself.
//
// Classification of record j: its kScOwnedPasses stamp for rdr_own::Pass::Scene3D (hook frame j - 1, scene_frame.h): decision Ours = OURS; GameWalks with why Hold on a sentinel hook frame =
// sentinel; anything else (no stamp: mode 0 / 1, or another reason) = walked.
// The hash covers the VALUES only: every row's `frame` / `guest_frame` member is zeroed before hashing (every row type is a flat POD with explicit padding, scene_record.h static_asserts), so a
// row that only carries a new frame number is not a change. The section map per bit is below (kSections); an input with no section of its own shares one (stated there).
// Off by default (--wwe2k14_rdr_scene_changes): Note returns at its first test.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "../renderer_game/scene_frame.h"
#include "../renderer_scene/record_io.h"
#include "../renderer_scene/record_all.h"   // S106 G4-2
#include "inputs_core.h"   // scene_inputs::kInputCount, kInputs (names)

namespace rdr_scene::live::inputchanges {

inline std::atomic<bool> g_on{false};
#ifdef RDR_S2_PLANTS
inline int g_changes_plant = 0;   // test only: 1 = the frame fields are NOT zeroed before hashing (a row carrying only a new frame number counts as a change)
#endif

// A 64-bit hash over bytes, 8 at a time (multiply-xorshift; not cryptographic, only "did these bytes change").
struct Hasher {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  void Word(uint64_t w) {
    h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
  }
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
      uint64_t w;
      std::memcpy(&w, b + i, 8);
      Word(w);
    }
    uint64_t tail = 0;
    if (i < n) std::memcpy(&tail, b + i, n - i);
    Word(tail ^ (uint64_t(n) << 56));
  }
};
template <class T>
inline void Row(Hasher& h, T r) {   // by value: the copy has its frame fields zeroed
  static_assert(std::is_trivially_copyable_v<T>, "a record row is a flat POD");
  bool zero = true;
#ifdef RDR_S2_PLANTS
  zero = g_changes_plant != 1;
#endif
  if (zero) {
    if constexpr (requires { r.frame; }) r.frame = {};
    if constexpr (requires { r.guest_frame; }) r.guest_frame = {};
  }
  h.Bytes(&r, sizeof r);
}
template <class T>
inline void Rows(Hasher& h, const std::vector<T>& v) {
  h.Word(v.size());
  for (const T& r : v) Row(h, r);
}

// The record section(s) that carry each input's value (scene_inputs::Input; the tables are record_io.h SceneTables).
inline uint64_t HashInput(int bit, const SceneTables& t) {
  Hasher h;
  h.Word(uint64_t(bit));
  switch (bit) {
    case scene_inputs::kInPostSettings: Rows(h, t.post_params); break;
    case scene_inputs::kInModelRuntime: Rows(h, t.model_runtime); break;           // wrestlers and arena models in one table (shared with bit 16)
    case scene_inputs::kInSphereOcc: Rows(h, t.sphere_occ); break;
    case scene_inputs::kInWrestlerPalette: Rows(h, t.bones); break;                 // every skinned object's palette (the wrestlers' are most of it)
    case scene_inputs::kInLightStates: Rows(h, t.light_states), Rows(h, t.light_refs), Rows(h, t.light_objects); break;
    case scene_inputs::kInArenaWorlds: Rows(h, t.arena_worlds); break;
    case scene_inputs::kInRingRows: Rows(h, t.ring_rows), Rows(h, t.ring_nodes); break;
    case scene_inputs::kInCasterEntries: Rows(h, t.caster_calls), Rows(h, t.caster_states), Rows(h, t.caster_nodes), Rows(h, t.caster_extras), Rows(h, t.caster_mats); break;
    case scene_inputs::kInCirclePoints: Rows(h, t.circle_shadows); break;
    case scene_inputs::kInPlaneConstants: Rows(h, t.plane_frames), Rows(h, t.plane_draws), Rows(h, t.plane_eyes); break;
    case scene_inputs::kInCrowdSnapshot: {
      uint64_t n = 0;
      for (const Object& o : t.objects)
        if (o.klass == rdr_adapter::DrawClass::Crowd) Row(h, o), ++n;
      h.Word(n);
      Rows(h, t.crowd_runs), Rows(h, t.crowd_members), Rows(h, t.crowd_props), Rows(h, t.crowd_singles), Rows(h, t.crowd_pictures);
      break;
    }
    case scene_inputs::kInParticles: {
      Rows(h, t.particles.batches), Rows(h, t.particles.particles);
      std::vector<ParticleStatus> st = t.particles.status;   // S103 MP: the info bits (the empty-pass marker) are not a value change: hashed out
      for (ParticleStatus& s : st) s.reasons &= ~uint32_t(rdr_scene::kPiInfoMask);
      Rows(h, st);
      break;
    }
    case scene_inputs::kInCamera: h.Word(t.has_camera), Row(h, t.camera); break;
    case scene_inputs::kInMaterialParams: Rows(h, t.param_overrides); break;       // wrestlers and arena models in one table (shared with bit 16)
    case scene_inputs::kInStageScreens: Rows(h, t.stage_fx); break;                // the stage's live parts, screens among them
    case scene_inputs::kInStagePools: Rows(h, t.stage_look); break;
    case scene_inputs::kInArenaModels: Rows(h, t.model_runtime), Rows(h, t.param_overrides); break;   // no table of its own (shares bits 1 and 13)
    case scene_inputs::kInStageClock: Rows(h, t.part_clocks); break;
    default: break;
  }
  return h.h;
}

enum class Kind : uint32_t { Ours = 0, Walked = 1, Sentinel = 2 };
inline Kind Classify(const SceneTables& t) {
  for (const OwnedStamp& s : t.owned_passes) {
    if (s.pass != uint8_t(rdr_own::Pass::Scene3D)) continue;
    if (s.decision == uint8_t(rdr_own::Decision::Ours)) return Kind::Ours;
    if (s.why_not == uint8_t(rdr_own::WhyNot::Hold) && scene_frame::IsSentinel(s.guest_frame)) return Kind::Sentinel;
    return Kind::Walked;
  }
  return Kind::Walked;
}

class Census {
 public:
  static constexpr int kBits = scene_inputs::kInputCount;
  void Note(const SceneTables& t) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const Kind k = Classify(t);
    uint64_t now[kBits];
    for (int b = 0; b < kBits; ++b) now[b] = HashInput(b, t);
    std::lock_guard<std::mutex> lock(mu_);
    ++frames_[uint32_t(k)];
    for (int b = 0; b < kBits; ++b) {
      const bool changed = have_ && now[b] != prev_[b];
      if (changed) ++changes_[b][uint32_t(k)];
      if (k == Kind::Sentinel && changed && still_[b] > 0) ++jumps_[b];   // stood still over the OURS frames before this sentinel, changed on the walk
      still_[b] = (k == Kind::Ours && have_ && !changed) ? still_[b] + 1 : 0;
      prev_[b] = now[b];
    }
    have_ = true;
  }
  // `[scene][changes] records ours O walked W (sentinel S); per input changes ours/walked/sentinel jumps: name o/w/s j, ...; fed by the walk: names|none; sentinel jumps: names|none`
  std::string Line() {
    std::lock_guard<std::mutex> lock(mu_);
    const auto U = [](uint64_t v) { return std::to_string((unsigned long long)v); };
    std::string s = "[scene][changes] records ours " + U(frames_[0]) + " walked " + U(frames_[1] + frames_[2]) + " (sentinel " + U(frames_[2]) + "); per input changes ours/walked/sentinel jumps:";
    for (int b = 0; b < kBits; ++b)
      s += std::string(b ? ", " : " ") + scene_inputs::kInputs[b].name + " " + U(changes_[b][0]) + "/" + U(changes_[b][1] + changes_[b][2]) + "/" + U(changes_[b][2]) + " " + U(jumps_[b]);
    s += "; fed by the walk:" + Names(true) + "; sentinel jumps:" + Names(false);
    return s;
  }
  // Test access.
  uint64_t Changes(int bit, Kind k) { std::lock_guard<std::mutex> lock(mu_); return changes_[bit][uint32_t(k)]; }
  uint64_t Jumps(int bit) { std::lock_guard<std::mutex> lock(mu_); return jumps_[bit]; }
  uint64_t Frames(Kind k) { std::lock_guard<std::mutex> lock(mu_); return frames_[uint32_t(k)]; }
  bool FedByWalk(int bit) { std::lock_guard<std::mutex> lock(mu_); return Fed(bit); }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    std::memset(prev_, 0, sizeof prev_), std::memset(changes_, 0, sizeof changes_), std::memset(jumps_, 0, sizeof jumps_), std::memset(still_, 0, sizeof still_), std::memset(frames_, 0, sizeof frames_);
    have_ = false;
  }

 private:
  // Fed by the walk = OURS records exist, the value never changed on one, and it changed on walked ones.
  bool Fed(int b) const { return frames_[0] > 0 && changes_[b][0] == 0 && (changes_[b][1] + changes_[b][2]) > 0; }
  std::string Names(bool fed) const {
    std::string s;
    for (int b = 0; b < kBits; ++b)
      if (fed ? Fed(b) : jumps_[b] > 0) s += std::string(" ") + scene_inputs::kInputs[b].name;
    return s.empty() ? " none" : s;
  }
  std::mutex mu_;
  uint64_t prev_[kBits] = {}, changes_[kBits][3] = {}, jumps_[kBits] = {}, still_[kBits] = {}, frames_[3] = {};
  bool have_ = false;
};
inline Census& The() {
  static Census c;
  return c;
}

}  // namespace rdr_scene::live::inputchanges
