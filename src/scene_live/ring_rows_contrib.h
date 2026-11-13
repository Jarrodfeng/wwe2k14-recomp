// The ring's per-call node rows as a live contributor to the scene record (Session 101, lane 194 = Ring D3 piece R2; docs\renderer\SKIP_GAME_WORK_MAP.md 16.3 / 16.7). Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause. Header-only, no game or SDK includes. A Contributor named "ring_rows", registered once at startup (wwe2k14_app.h OnPostSetup, with --wwe2k14_scene_records
// --wwe2k14_scene_light_states --wwe2k14_rdr_ring_d3; the model-entry hook returns at its first check until then). Emit appends every closed frame it has not emitted yet, oldest first: the
// RingRows entries as the hook wrote them (each stamped with the HOOK's frame, never the record's), their node rows rebased into the record's pool. Registered AFTER the light-state contributor,
// so a frame's LightRefs are already in the record when this one runs: the cross-check (CrossCheckLightRefs) compares the two readers' per-object call counts.
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "ring_rows_hook.h"

namespace rdr_scene::live::ringrows {

class RingRowsContributor : public Contributor {
 public:
  const char* Name() const override { return "ring_rows"; }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    tracker.CloseBefore(fi.guest_frame);   // lane 152 (B1)
    bool any = false;
    while (tracker.TakeNext(scratch_)) {
      any = true;
      const uint32_t base = uint32_t(t.ring_nodes.size());
      const size_t first = t.ring_rows.size();
      t.ring_nodes.insert(t.ring_nodes.end(), scratch_.nodes.begin(), scratch_.nodes.end());
      for (RingRows r : scratch_.rows) {
        r.first_node += base;
        t.ring_rows.push_back(r);
        covered_calls_ += (r.n_nodes > 0) ? 1 : 0;
      }
      ++frames_emitted_;
      if (!scratch_.rows.empty()) CrossCheckLightRefs(t.ring_rows, first, t.light_refs, uint32_t(scratch_.frame), cross_);
    }
    if (!any) ++c.stale;   // the hook closed no new frame since the last record
    const Stats s = tracker.GetStats();
    const uint64_t refused = s.unreadable + s.count_unknown + s.overflow;   // late frames and calls of dropped frames are NOT failures (lane 152 V1): they stay in the ring_rows census line only
    c.unknown += uint32_t(refused - last_refused_);
    last_refused_ = refused;
  }
  // The log line that decides the verify arm (docs\renderer\SKIP_GAME_WORK_MAP.md 16.7): calls recorded vs the frames' expected set, nothing refused, the LightRef counts equal.
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[ring_rows calls " + std::to_string(s.calls) + ", ring " + std::to_string(s.ring_calls) + ", recorded " + std::to_string(s.recorded) + ", covered " + std::to_string(covered_calls_) + ", rows " +
           std::to_string(s.rows_copied) + ", frames " + std::to_string(s.frames) + ", full " + std::to_string(s.frames_full) + ", unreadable " + std::to_string(s.unreadable) + ", count_unknown " +
           std::to_string(s.count_unknown) + ", capped " + std::to_string(s.capped) + ", foreign_r5 " + std::to_string(s.foreign_r5) + ", forwarded " + std::to_string(s.forwarded) + ", overflow " +
           std::to_string(s.overflow) + ", late " + std::to_string(s.late) + ", old_calls " + std::to_string(s.old_calls) + ", table_missing " + std::to_string(s.table_missing) + ", alpha_unread " +
           std::to_string(s.alpha_unread) + ", ref_objects " + std::to_string(cross_.objects) + ", ref_equal " + std::to_string(cross_.equal) + ", ref_differ " + std::to_string(cross_.differ) +
           ", ref_unjoined_frames " + std::to_string(cross_.unjoined_frames) + ", classes";
    for (uint32_t k = 1; k < kRcCount; ++k) out += std::string(" ") + ClassName(uint8_t(k)) + " " + std::to_string(s.by_class[k]);
    out += "]";
  }
  Tracker tracker;
  RefCross Cross() const { return cross_; }

 private:
  FrameBuffer scratch_;
  uint64_t last_refused_ = 0, covered_calls_ = 0, frames_emitted_ = 0;
  RefCross cross_;
};

inline RingRowsContributor& Rows() {
  static RingRowsContributor r;
  return r;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterRingRows() {   // idempotent; the hooks do nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&Rows());
}
inline bool RingRowsRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::ringrows
