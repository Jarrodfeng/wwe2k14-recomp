// The plane shadow chain as a live contributor to the scene record (Session 101, lane 139; docs/renderer/SHADOW_PLANE.md; sections kScPlaneFrames / kScPlaneDraws).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes. A Contributor named "plane": Emit appends every frame block and draw the hook gathered since the last
// record (each stamped with its own guest frame). Registered once at startup, only with --wwe2k14_scene_records plus a plane switch (wwe2k14_app.h).
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "plane_hook.h"

namespace rdr_scene::live::plane {

// The writer checks its own tables (live::Validate does not look at them): a table that does not check out is dropped, only the plane tables, and counted. Returns true when dropped.
inline bool DropIfBad(SceneTables& t, Tracker* tracker = nullptr) {
  if (PlaneTablesProblem(t).empty()) return false;
  t.plane_frames.clear();
  t.plane_draws.clear();
  if (tracker) tracker->CountDropped();
  return true;
}

class PlaneContributor : public Contributor {
 public:
  const char* Name() const override { return "plane"; }
  void Emit(const FrameInfo&, SceneTables& t, Counters& c) override {
    tracker.Take(frames_, draws_);
    if (draws_.empty() && frames_.empty()) {
      ++empty_emits_;   // the hook gathered nothing since the last record: this record carries no plane tables (the contributor's own count, apart from the tracker's)
      return;
    }
    t.plane_frames.insert(t.plane_frames.end(), frames_.begin(), frames_.end());
    t.plane_draws.insert(t.plane_draws.end(), draws_.begin(), draws_.end());
    c.objects += uint32_t(draws_.size());
    DropIfBad(t, &tracker);
    frames_.clear();
    draws_.clear();
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[plane calls " + std::to_string(s.calls) + ", mask rows " + std::to_string(s.rows_mask) + ", blur rows " + std::to_string(s.rows_blur) + ", frames " + std::to_string(s.frames) +
           ", mask draws " + std::to_string(s.mask_draws) + ", blur draws " + std::to_string(s.blur_draws) + ", incomplete frames " + std::to_string(s.incomplete_frames) + ", orphans " + std::to_string(s.orphans) +
           ", unreadable " + std::to_string(s.unreadable) + ", overflow " + std::to_string(s.overflow) + ", tables dropped " + std::to_string(s.dropped_tables) + ", empty emits " + std::to_string(empty_emits_) + "]";
  }
  Tracker tracker;

 private:
  std::vector<PlaneFrame> frames_;
  std::vector<PlaneDraw> draws_;
  uint64_t empty_emits_ = 0;
};

inline PlaneContributor& The() {
  static PlaneContributor c;
  return c;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterPlane() {   // idempotent; the hooks do nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&The());
}
inline bool PlaneRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::plane
