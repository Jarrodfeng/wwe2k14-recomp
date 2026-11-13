// The wrestlers' sphere occluders as a live contributor to the scene record (Session 101, lane 242 = H3; docs\renderer\SKIP_GAME_WORK_MAP.md 18.8). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes. A Contributor named "spheres", registered once at startup (wwe2k14_app.h OnPostSetup, with --wwe2k14_scene_records --wwe2k14_scene_light_states
// --wwe2k14_rdr_caster_d3, the switches of the caster record it rides on; Android never registers it; until then the hook returns at its first check). Emit appends every closed hook frame it has not
// emitted yet, oldest first: the SphereOccRow entries as the hook wrote them, each stamped with the HOOK's guest frame (never the record's; the record's header runs one frame ahead of the hook's
// stamp, caster_contrib.h kHookToRecord).
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "sphere_hook.h"

namespace rdr_scene::live::spheres {

class SphereContributor : public Contributor {
 public:
  const char* Name() const override { return "spheres"; }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    tracker.CloseBefore(fi.guest_frame);
    bool any = false;
    while (tracker.TakeNext(scratch_)) {
      any = true;
      t.sphere_occ.insert(t.sphere_occ.end(), scratch_.rows.begin(), scratch_.rows.end());
      c.objects += uint32_t(scratch_.rows.size());
      ++frames_emitted_;
    }
    if (!any) ++c.stale;
    const Stats s = tracker.GetStats();
    const uint64_t refused = s.unreadable + s.bad_count + s.overflow;   // N2: a row without a skeleton IS recorded (object 0, kSoSkeleton clear), so it is not counted unknown; the census shows no_skeleton
    c.unknown += uint32_t(refused - last_refused_);
    last_refused_ = refused;
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[spheres calls " + std::to_string(s.calls) + ", recorded " + std::to_string(s.recorded) + ", replaced " + std::to_string(s.replaced) + ", early_out " + std::to_string(s.early_out) +
           ", unreadable " + std::to_string(s.unreadable) + ", bad_count " + std::to_string(s.bad_count) + ", no_skeleton " + std::to_string(s.no_skeleton) + ", skipped_sphere " +
           std::to_string(s.skipped_sphere) + ", overflow " + std::to_string(s.overflow) + ", late " + std::to_string(s.late) + ", frames " + std::to_string(s.frames) + ", emitted " +
           std::to_string(frames_emitted_) + "]";
  }
  Tracker tracker;

 private:
  FrameBuffer scratch_;
  uint64_t last_refused_ = 0, frames_emitted_ = 0;
};

inline SphereContributor& Spheres() {
  static SphereContributor r;
  return r;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterSpheres() {   // idempotent; the hook does nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&Spheres());
}
inline bool SpheresRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::spheres
