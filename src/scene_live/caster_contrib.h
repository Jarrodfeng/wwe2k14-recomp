// The caster passes' per-entry record as a live contributor to the scene record (Session 101, lane 202 = Caster D3 piece E2/E3 reader side; docs\renderer\SKIP_GAME_WORK_MAP.md 17.3 / 17.7).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game or SDK includes. A Contributor named "casters", registered once at startup (wwe2k14_app.h OnPostSetup, with
// --wwe2k14_scene_records --wwe2k14_scene_light_states --wwe2k14_rdr_caster_d3; the pass hooks return at their first check until then). Emit appends every closed frame it has not emitted yet,
// oldest first: the CasterCall entries as the hook wrote them (each stamped with the HOOK's frame, never the record's) with their state indices rebased into the record's states.
// Registered AFTER the light-state contributor and the wrestlers' (the registration order in wwe2k14_app.h), so the frame's LightRefs and bones are in the record when this one runs: the census
// compares the entry's camera with the colour calls' and its palette hash with the wrestler record's bones (caster_core.h CrossCheckCalls).
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "caster_hook.h"

namespace rdr_scene::live::casters {

class CasterContributor : public Contributor {
 public:
  const char* Name() const override { return "casters"; }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    tracker.CloseBefore(fi.guest_frame);   // lane 152 (B1)
    bool any = false;
    while (tracker.TakeNext(scratch_)) {
      any = true;
      const uint32_t base = uint32_t(t.caster_states.size());
      const size_t first = t.caster_calls.size();
      const uint32_t node_base = uint32_t(t.caster_nodes.size());   // lane 217
      t.caster_states.insert(t.caster_states.end(), scratch_.states.begin(), scratch_.states.end());
      const uint32_t mat_base = uint32_t(t.caster_mats.size());   // lane 226
      t.caster_nodes.insert(t.caster_nodes.end(), scratch_.nodes.begin(), scratch_.nodes.end());
      t.caster_mats.insert(t.caster_mats.end(), scratch_.mats.begin(), scratch_.mats.end());
      if (scratch_.extras.size() == scratch_.calls.size()) t.caster_extras.insert(t.caster_extras.end(), scratch_.extras.begin(), scratch_.extras.end());   // lane 221: parallel to the calls
      for (CasterCall r : scratch_.calls) {
        if (r.state != kCasterNoState) r.state += base;
        if (r.flags & kCcNodeHashes) r.node_first += node_base;
        if (r.flags & kCcMats) r.node_first += mat_base;
        t.caster_calls.push_back(r);
      }
      ++frames_emitted_;
      c.objects += uint32_t(scratch_.calls.size());
      if (!scratch_.calls.empty()) {
        CrossCheckCalls(t, first, uint32_t(scratch_.frame), fi.guest_frame, kHookToRecord, cross_);
        CrossCheckExtras(t, first, uint32_t(scratch_.frame), cross_);   // lane 221
        CrossCheckNodes(t, first, uint32_t(scratch_.frame), stash_, kStash, cross_);   // lane 217: the bones of the record whose header is the call's stamp, stashed at ITS Emit
      }
    }
    if (!any) ++c.stale;   // the hook closed no new frame since the last record
    StashBones(t, fi.guest_frame, stash_[stash_next_++ % kStash]);   // lane 217 (after the cross-checks: a record's own bones serve LATER records' calls)
    const Stats s = tracker.GetStats();
    const uint64_t refused = s.unreadable + s.bad_slot + s.state_overflow + s.overflow;   // late frames and calls of dropped frames are NOT failures (lane 152 V1): they stay in the casters census line only
    c.unknown += uint32_t(refused - last_refused_);
    last_refused_ = refused;
  }
  // The log line that decides the verify arm (SKIP_GAME_WORK_MAP.md 17.7): entries per pass, nothing refused, what R at the entry is.
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[casters calls " + std::to_string(s.calls) + ", recorded " + std::to_string(s.recorded) + ", map " + std::to_string(s.by_pass[kCpMap]) + ", plane " + std::to_string(s.by_pass[kCpPlane]) + ", rim " +
           std::to_string(s.by_pass[kCpRim]) + ", frames " + std::to_string(s.frames) + ", frames_map_rim " + std::to_string(tracker.FramesWithAllPasses()) + ", states " + std::to_string(s.appended) + ", same " +
           std::to_string(s.same_as_last) + ", found " + std::to_string(s.found) + ", collapsed " + std::to_string(s.collapsed) + ", unreadable " + std::to_string(s.unreadable) + ", bad_slot " +
           std::to_string(s.bad_slot) + ", state_overflow " + std::to_string(s.state_overflow) + ", overflow " + std::to_string(s.overflow) + ", late " + std::to_string(s.late) + ", old_calls " +
           std::to_string(s.old_calls) + ", no_palette " + std::to_string(s.no_palette) + ", pal_unreadable " + std::to_string(s.pal_unreadable) + ", capped " + std::to_string(s.capped) + ", foreign_r5 " +
           std::to_string(s.foreign_r5) + ", r5_zero " + std::to_string(s.r5_zero) + ", collapse_unread " + std::to_string(s.collapse_unread) + ", cam_eq_colour " + std::to_string(cross_.cam_eq_colour) +
           ", cam_differ " + std::to_string(cross_.cam_differ) + ", pal_equal " + std::to_string(cross_.pal_equal) + ", pal_differ " + std::to_string(cross_.pal_differ) + ", pal_not_in_record " +
           std::to_string(cross_.pal_not_in_record) + ", pal_other_frame " + std::to_string(cross_.pal_other_frame) + ", node_hashes " + std::to_string(s.node_hashes) + ", node_calls " +
           std::to_string(cross_.node_calls) + ", node_equal " + std::to_string(cross_.node_calls_equal) + ", node_differ " + std::to_string(cross_.node_calls_differ) + ", node_no_bones " +
           std::to_string(cross_.node_no_bones) + ", node_not_in_record " + std::to_string(cross_.node_not_in_record) + ", node_short " + std::to_string(cross_.node_short) + ", node_first_differ " +
           (cross_.node_first_differ_min == 0xFFFFFFFFu ? std::string("none") : std::to_string(cross_.node_first_differ_min)) + ", differing nodes by block {" + NodeBlocksText(cross_) + "}, plane_mats " + std::to_string(s.plane_mats) + " calls " + std::to_string(s.plane_calls_mats) + " unreadable " + std::to_string(s.plane_mats_unreadable) + ", extras product " + std::to_string(s.extra_product) + " depth " + std::to_string(s.extra_depth) + " pose " +
           std::to_string(s.extra_pose) + " unreadable " + std::to_string(s.extra_unreadable) + ", product_eq_r map/plane/rim " + std::to_string(cross_.prod_eq_r[kCpMap]) + "/" + std::to_string(cross_.prod_eq_r[kCpPlane]) + "/" +
           std::to_string(cross_.prod_eq_r[kCpRim]) + " differ " + std::to_string(cross_.prod_differ_r[kCpMap]) + "/" + std::to_string(cross_.prod_differ_r[kCpPlane]) + "/" + std::to_string(cross_.prod_differ_r[kCpRim]) +
           " missing " + std::to_string(cross_.prod_missing) + "]";
  }
  Tracker tracker;
  Cross Crossed() const { return cross_; }

  // The record's header runs one frame ahead of the hook's stamp (lane 152 (12) / kHookToStreamOffset); a palette is compared with the record's bones only in that pairing.
  static constexpr uint32_t kHookToRecord = 1;

 private:
  static constexpr size_t kStash = 8;   // lane 217: records' bone hashes kept for the per-node compare
  BoneStash stash_[kStash];
  size_t stash_next_ = 0;
  FrameBuffer scratch_;
  uint64_t last_refused_ = 0, frames_emitted_ = 0;
  Cross cross_;
};

inline CasterContributor& Casters() {
  static CasterContributor r;
  return r;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterCasters() {   // idempotent; the hooks do nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&Casters());
}
inline bool CastersRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::casters
