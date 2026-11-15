// The circle blob shadows as a live contributor to the scene record (Session 101, lane 136; docs/renderer/SHADOW_CIRCLES.md; sections kScCircleShadows / kScCircleMesh).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes. A Contributor named "circles": Emit appends every entry the hook gathered since the
// last record (each stamped with its own guest frame) and, with them, the disc mesh. Registered once at startup, only with --wwe2k14_scene_records plus a circles switch (wwe2k14_app.h).
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "circle_hook.h"
#include "tail_latch.h"  // S102 U1

namespace rdr_scene::live::circles {

class CircleContributor : public Contributor {
 public:
  const char* Name() const override { return "circles"; }
  bool Latch(const FrameInfo& fi) override {   // S102 U1 (tail_latch.h): Emit's take, while the game threads are parked
    tracker.Take(scratch_, &latch_mesh_, latch_mesh_present_, /*always_mesh=*/true);
    latch_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    bool mesh_present = false;
    CircleMesh mesh;
    if (latch_.Use(fi.guest_frame, Name())) mesh = latch_mesh_, mesh_present = latch_mesh_present_;   // S102 U1: what the latch took (scratch_ holds its entries)
    else tracker.Take(scratch_, &mesh, mesh_present, /*always_mesh=*/true);
    if (scratch_.empty()) {
      ++c.stale;   // the hook gathered no entry since the last record: this record carries no circle tables
      return;
    }
    t.circle_shadows.insert(t.circle_shadows.end(), scratch_.begin(), scratch_.end());
    c.objects += uint32_t(scratch_.size());
    if (mesh_present) t.circle_mesh.push_back(mesh);
    else ++c.unknown;   // entries without a mesh: the mesh could not be read from the object (counted; the renderer keeps the stream's draws)
    scratch_.clear();
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[circles calls " + std::to_string(s.calls) + ", viewproj " + std::to_string(s.viewproj) + ", params " + std::to_string(s.params) + ", entries " + std::to_string(s.entries) +
           ", orphans " + std::to_string(s.orphans) + ", unreadable " + std::to_string(s.unreadable) + ", bad_slot " + std::to_string(s.bad_slot) + ", overflow " + std::to_string(s.overflow) +
           ", mesh reads " + std::to_string(s.mesh_reads) + " ok " + std::to_string(s.mesh_ok) + " bad " + std::to_string(s.mesh_bad) + "]";
  }
  Tracker tracker;

 private:
  std::vector<CircleShadow> scratch_;
  LatchMark latch_;   // S102 U1
  CircleMesh latch_mesh_;
  bool latch_mesh_present_ = false;
};

inline CircleContributor& The() {
  static CircleContributor c;
  return c;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterCircles() {   // idempotent; the op-21 hook does nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&The());
}
inline bool CirclesRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::circles
