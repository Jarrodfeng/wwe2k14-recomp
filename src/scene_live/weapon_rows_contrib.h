// Weapons and props: the per-call rows as a live contributor to the scene record (Session 103, lane PW-2; contract src\renderer_scene\weapon_rows.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes. A Contributor named "weapon_rows", registered once at startup (wwe2k14_app.h OnPostSetup, with the scene records and --wwe2k14_scene_light_states: the same rule as the
// arena's worlds); the model-entry hook returns at its first check until then. Emit takes every closed frame it has not emitted yet but writes ONLY THE NEWEST of them (PW review fix 7: an older frame's calls are
// never drawn, and a newest frame with no weapon call must not leave an older frame's weapons on screen; the dropped frames are counted): the WeaponCall rows as the tracker wrote them, their node rows rebased into the
// record's kScWeaponNodes pool (section 61). Read only.
#pragma once
#include <atomic>
#include <cstring>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/rows_change_colour.h"   // S108 THB: section 67 (the entrance props' colours)
#include "../renderer_scene/weapon_rows.h"   // S106 G4-2
#include "tail_latch.h"  // S102 U1
#include "weapon_rows_core.h"

namespace rdr_scene::live::weaponrows {

class WeaponRowsContributor : public Contributor {
 public:
  const char* Name() const override { return "weapon_rows"; }
  bool Latch(const FrameInfo& fi) override { return latch_.Take(tracker, fi); }   // S102 U1 (tail_latch.h)
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    latch_.Begin(tracker, fi, Name());
    bool any = false;
    while (latch_.Next(tracker, scratch_)) {   // oldest first; only the last (the newest closed frame) is written
      if (any) dropped_frames_ += 1, dropped_calls_ += newest_.calls.size();
      any = true;
      std::swap(newest_, scratch_);
      ++frames_emitted_;
    }
    for (const auto& ev : tracker.events.Snapshot()) {   // S103 BLF review 4: every live event prop model is a section-59 row (flag kSlotFlagEvent) so its load starts before its first draw
      rdr_scene::weapons::WeaponSlot s;
      s.id = ev.second.id, s.slot = ev.second.k, s.main = ev.first, s.flags = rdr_scene::weapons::kSlotFlagEvent;
      t.weapon_slots.push_back(s);
    }
    {   // S103 CGF: every live stage structure model (the holder's members as of the last pull) is a section-59 row (flag kSlotFlagStage; id = PACH, slot = k) so its bgEtc.pac load starts before its first draw
      const StageView sv = tracker.StageSnapshot();
      for (const StageMember& m : sv.members) {
        rdr_scene::weapons::WeaponSlot s;
        s.id = rdr_scene::weapons::StageId(sv.pach), s.slot = m.k,   // review 10: kStageIdBase + PACH (an older reader would take a bare 0..7 for an m2 weapon id) s.main = m.model, s.flags = rdr_scene::weapons::kSlotFlagStage;
        t.weapon_slots.push_back(s);
      }
    }
    if (!any) return void(++c.stale);
    const uint32_t base = uint32_t(t.weapon_nodes.size());
    const size_t prop_rows_first = t.change_colours.size();   // S108 THB: this writer's own section-67 rows start here
    t.weapon_nodes.insert(t.weapon_nodes.end(), newest_.nodes.begin(), newest_.nodes.end());
    for (rdr_scene::weapons::WeaponCall r : newest_.calls) {
      r.node_first += base;
      t.weapon_calls.push_back(r);
      if (!rdr_scene::weapons::IsEventCall(r.slot_flags)) continue;
      // S108 THB: the entrance prop's colours (the Threads editor's live ones, or the saved set's latched at the recolour block): ONE section-67 row per prop model, keyed by the model address (= the call's object).
      bool have_row = false;
      for (size_t i = prop_rows_first; i < t.change_colours.size(); ++i) have_row = have_row || t.change_colours[i].object == r.object;
      if (have_row) continue;
      propcolour::Out pc;
      if (!propcolour::TheStore().Get(r.object, fi.guest_frame, pc)) continue;
      rdr_scene::ChangeColourRow row;
      row.object = r.object;
      row.flags = rdr_scene::kCcProp | (pc.editor ? rdr_scene::kCcEditor : 0u);
      std::memcpy(row.colours, pc.colours, propcolour::kColourBytes);
      t.change_colours.push_back(row);
      propcolour::TheStore().NoteRow();
    }
  }
  // `[scene][weapon_rows] calls N objects M, named but never drawn K` (cumulative; the last closed frame and the refusals after it). (PW-1's slot census is `[scene][weapons]`.) "other passes" = calls of a weapon's
  // model that were not its own slot-13 submit (slot 19 sub_82C13A80 / slot 32 sub_82C13EC8 passes): counted, never recorded, never a second chair.
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[scene][weapon_rows] calls " + std::to_string(s.recorded) + " objects " + std::to_string(s.objects) + ", named but never drawn " + std::to_string(s.never_drawn) + " (named " +
           std::to_string(s.named) + ", last frame named " + std::to_string(s.last_named) + " never " + std::to_string(s.last_never) + ", model calls " + std::to_string(s.calls) + ", weapon calls " +
           std::to_string(s.weapon_calls) + ", rows " + std::to_string(s.rows_copied) + ", frames " + std::to_string(s.frames) + ", unreadable " + std::to_string(s.unreadable) + ", count_unknown " +
           std::to_string(s.count_unknown) + ", capped " + std::to_string(s.capped) + ", overflow " + std::to_string(s.overflow) + ", late " + std::to_string(s.late) + ", old_calls " +
           std::to_string(s.old_calls) + ", member_reads " + std::to_string(s.member_reads) + ", mgr_missing " + std::to_string(s.mgr_missing) + ", slots_refused " + std::to_string(s.slots_refused) + ", rigid " + std::to_string(s.rigid) + ", rigid_node_assumed " + std::to_string(s.rigid_node_assumed) + ", slots_unreadable " +
           std::to_string(s.slots_unreadable) + ", bad_vtable_calls " + std::to_string(s.bad_vtable_calls) + ", other passes " + std::to_string(s.other_pass) + ", older frames dropped " + std::to_string(dropped_frames_) + " (" + std::to_string(dropped_calls_) + " calls))" +
           ", stage pulls " + std::to_string(s.stage_pulls) + " frames " + std::to_string(s.stage_frames) + " type " + std::to_string(s.stage_type) + " models " + std::to_string(s.stage_models) + " calls " + std::to_string(s.stage_calls) + " (slot32 " + std::to_string(s.stage_slot32) +
           " own " + std::to_string(s.stage_own) + " [cage-node " + std::to_string(s.stage_own_cage) + " other " + std::to_string(s.stage_own_other) + "]) recheck_fail " + std::to_string(s.stage_recheck_fail) + " capped " + std::to_string(s.stage_capped) + " rigid_bounded " + std::to_string(s.stage_rigid_bounded) + ", shadow_calls " + std::to_string(s.shadow_calls) + " shadow_props_calls " + std::to_string(s.shadow_props_calls) + ", event rows in place " + std::to_string(s.event_rows_in_place) + " of replaced " + std::to_string(s.event_replaced) + ", largest call " + std::to_string(s.max_rows) + " rows (" + (s.max_rows_event ? "event prop " : "id ") + std::to_string(s.max_rows_id) + " k " + std::to_string(s.max_rows_k) + "), model_lock " + tracker.LockText();   // S108 FAN: capped = counts over kWeaponMaxNodes (refused), overflow = pool full (refused); never cut
    // S103 GBR (the step check reads these): the gm set objects (membership rebuilds, models, recorded calls, non-submit calls, non-zero pass, re-check failures), the blood splats (recorded, live records at the last snapshot, dead records skipped,
    // calls with no live record left, non-submit, non-zero pass, pool unreadable) and the event list's resets (scene changes, entries cleared by them, entries pruned for a stale vtable)
    uint64_t evc = 0, evp = 0, evo = 0;
    tracker.events.Resets(evc, evp, evo);
    out += "; gm pulls " + std::to_string(s.gm_pulls) + " models " + std::to_string(s.gm_models) + " calls " + std::to_string(s.gm_calls) + " other_pass " + std::to_string(s.gm_other_pass) + " pass_skip " + std::to_string(s.gm_pass_skip) + " recheck_fail " + std::to_string(s.gm_recheck_fail) +
           "; blood calls " + std::to_string(s.blood_calls) + " frames " + std::to_string(s.blood_frames) + " live " + std::to_string(s.blood_live) + " dead_skipped " + std::to_string(s.blood_dead_skipped) + " unmatched " + std::to_string(s.blood_unmatched) + " other_pass " +
           std::to_string(s.blood_other_pass) + " pass_skip " + std::to_string(s.blood_pass_skip) + " unreadable " + std::to_string(s.blood_unreadable) + "; event list scene_changes " + std::to_string(s.ev_scene_changes) + " cleared " + std::to_string(evc) + " pruned " +
           std::to_string(evp) + " pub_overflow " + std::to_string(evo);
    // S103 LDR (93db71f17): the stage path latch's counters: requests seen, of those a ladder PACH, a ladder-looking path with no PACH (the step check reads them; the lint counter_reader wants a reader)
    out += "; " + propcolour::TheStore().CensusLine();   // S108 THB: the entrance props' colours (the editor's reads, the saved sets' latches, the rows, the draws)
    out += "; stage paths " + std::to_string(tracker.stage_paths.load(std::memory_order_relaxed)) + " ladder " + std::to_string(tracker.stage_paths_ladder.load(std::memory_order_relaxed)) + " unmapped " + std::to_string(tracker.stage_paths_unmapped.load(std::memory_order_relaxed));
  }
  Tracker tracker;
  const TrackerLatch<FrameBuffer, kRing>& Latched() const { return latch_; }   // S102 U1 (tests)

 private:
  TrackerLatch<FrameBuffer, kRing> latch_;
  FrameBuffer scratch_, newest_;
  uint64_t frames_emitted_ = 0, dropped_frames_ = 0, dropped_calls_ = 0;
};

inline WeaponRowsContributor& Rows() {
  static WeaponRowsContributor r;
  return r;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterWeaponRows() {   // idempotent; the hooks do nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&Rows());
}
inline bool WeaponRowsRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::weaponrows
