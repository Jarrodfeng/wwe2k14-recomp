// The per-draw light state as a live contributor to the scene record (Session 101, lane 109 = LS1; docs\renderer\PER_DRAW_LIGHTS.md section 3 (c),
// docs\renderer\SCENE_RECORD.md sections kScLightStates / kScLightRefs). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game or SDK
// includes. A Contributor named "lights": Emit appends every closed frame it has not emitted yet (oldest first, light_state_hook.h) with the state indices
// rebased; each LightRef carries the hook's own frame. Registered once at startup, only with --wwe2k14_scene_light_states (wwe2k14_app.h OnPostSetup,
// which needs --wwe2k14_scene_records too); the model-entry hook returns at its first check otherwise.
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "light_state_hook.h"

namespace rdr_scene::live::lights {

class LightStateContributor : public Contributor {
 public:
  const char* Name() const override { return "lights"; }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    tracker.CloseBefore(fi.guest_frame);   // lane 152 (B1)
    bool any = false;
    while (tracker.TakeNext(scratch_)) {
      any = true;
      const uint32_t base = uint32_t(t.light_states.size());
      t.light_states.insert(t.light_states.end(), scratch_.states.begin(), scratch_.states.end());
      for (LightRef r : scratch_.refs) {
        r.state += base;
        t.light_refs.push_back(r);
      }
      c.objects += uint32_t(scratch_.refs.size());
    }
    if (!any) ++c.stale;  // the hook closed no new frame since the last record: the record carries no light tables this frame
    const Stats s = tracker.GetStats();  // refusals show in the census as "unknown" fields (nothing recorded for those calls)
    c.unknown += uint32_t(s.unreadable + s.bad_slot + s.state_overflow + s.ref_overflow + s.late + s.old_calls - last_refused_);
    last_refused_ = s.unreadable + s.bad_slot + s.state_overflow + s.ref_overflow + s.late + s.old_calls;
  }
  // The tracker's totals in the census line (F4): calls, how many reused the slot's last state, new states, and every refusal by name.
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[lights calls " + std::to_string(s.calls) + ", same " + std::to_string(s.same_as_last) + ", found " + std::to_string(s.found) + ", new " +
           std::to_string(s.appended) + ", frames " + std::to_string(s.frames) + ", unreadable " + std::to_string(s.unreadable) + ", bad_slot " +
           std::to_string(s.bad_slot) + ", null_block " + std::to_string(s.null_block) + ", state_overflow " + std::to_string(s.state_overflow) +
           ", ref_overflow " + std::to_string(s.ref_overflow) + ", late " + std::to_string(s.late) + ", old_calls " + std::to_string(s.old_calls) + "]";
  }
  Tracker tracker;

 private:
  FrameBuffer scratch_;  // reused across Emit calls (its buffers cycle through the tracker's ring)
  uint64_t last_refused_ = 0;
};

inline LightStateContributor& States() {
  static LightStateContributor s;
  return s;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterLightStates() {  // idempotent; the model-entry hook does nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&States());
}
inline bool LightStatesRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::lights
