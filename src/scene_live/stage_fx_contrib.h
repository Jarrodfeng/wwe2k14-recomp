// The stage's live writers as a contributor to the scene record (S103 lane C2; docs\renderer\STAGE_LIVE_WRITERS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes: the game glue (stage_fx_glue.h) gives it the guest reader and the stage object; the offline unit (tools\scene\stage_fx_unit.cpp) gives it a fake guest.
//
// It writes the StageFxRow table lane C's appliers read (record section kScStageFx, id 39), from the UPDATE side, never from a walk (the walks of landing 1 are skipped):
//   * kFxSlotWorld: one row per MOVING arena slot (the arena text's m(N) slots), the slot's world read at the frame's LATCH from the slot's model object (stage_fx_core.h: where the object is, which
//     matrix, what is unknown). This is the join the kScArenaWorlds rows lacked: they key by the engine object, and the arena's slot objects carry engine_objects = 0; here the join is BY SLOT, through
//     the stage object's own slot -> model array.
//   * kFxScreen: one row per screen the screen source names (key 800-809, serial, picture size); `movie` = the index of that screen's Movie in the record (the arena contributor's TronPicture target of
//     the same key). The source of the serial / size is the movie owner (installed by the glue; none installed = no rows, counted `screens_unbound`).
//   * kFxFlash: no writer: the game's flash placement is not found yet (STAGE_LIVE_WRITERS.md section 4 names the trace).
// Read only: nothing is written to the guest. Off by default and free when off: with --wwe2k14_rdr_scene = 0 (every player's default) the latch reads nothing and Emit writes nothing.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../renderer_game/scene_frame.h"
#include "../renderer_scene/rows_arena_world.h"   // S106 G4-2
#include "../renderer_scene/rows_part_clock.h"
#include "../renderer_scene/rows_stage_fx.h"
#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/stage_look.h"   // S103 M: Entrance(t), the front end's own look signal
#include "../renderer_scene/stage_slot_vis.h"   // S103 WSL: the game's per-slot look gate (G5)
#include "../renderer_scene/car_arena_rows.h"   // S103 CAR: a created arena's part indices row (ReadCarSel / CarToRows)
#include "car_paint.h"   // S108 CAP: a created arena's painted textures, read at the latch
#include "../renderer_scene/stage_posed.h"   // S110 BARRICADE: the slots the game draws posed (palette rows)
#include "../renderer_scene/stage_show.h"   // S103 SCF: the game's per-screen / per-art-slot show bits
#include "inputs_core.h"   // S103 M (B2): kInStageScreens, Capture
#include "arena_id_core.h"   // S103 CAE: kStageArenaIdOffset, kMaxArenaId
#include "stage_fx_core.h"
#include "part_clock_read.h"   // S103 E5
#include "stage_clock_flag.h"   // S103 E5
#include "tail_latch.h"   // LatchMark
#include "latch_period.h"   // S111 CUTREACH2: OfPalette (record r takes latch r - 1's model-class set: slot rows, vis, show, CAR, part clocks)

namespace rdr_scene::live::stagefx {

// A screen the movie owner shows: key 800-809, the picture's serial (a new number = new pixels) and size.
struct ScreenSource {
  uint16_t key = 800;
  uint32_t serial = 0, w = 0, h = 0;
};

class StageFxContributor final : public Contributor {
 public:
  const char* Name() const override { return "stage_fx"; }

  // The glue sets these once at start (null in a test that sets its own).
  void SetReader(ReadFn r) { read_ = r; }
  using MovingFn = void (*)(std::vector<uint16_t>& out);   // fills `out` (cleared by the callee): no allocation per frame once the capacity is there
  void SetMoving(MovingFn m) { moving_ = m; }
  using ArenaFn = int32_t (*)();                            // the published arena id (-1 = unknown)
  void SetArenaFn(ArenaFn a) { arena_fn_ = a; }
  using ScreensFn = size_t (*)(ScreenSource* out, size_t cap);
  void SetScreens(ScreensFn s) { screens_ = s; }
  // S103 M: the live screens' census text (screen_planes_contrib.h CensusLine), appended to this contributor's census line.
  using CensusFn = void (*)(std::string& out);
  void SetExtraCensus(CensusFn c) { extra_census_ = c; }
  // S103 M (B2, DECISION SIDE): the input "screens" (scene_inputs::kInStageScreens) is stamped here, at the latch's Emit, ONLY when every stage screen that has a Movie in the record AND is shown in this look
  // (`shown`: arena::ScreenShown) has a picture row this frame. A shown screen without a row (unbound, no frame yet, a frame of the wrong size, stale) leaves the input unstamped: the frame is not decided
  // ours and stays the game's. `bound` only tells the two causes apart for the census (unbound key with a Movie, counted per frame). No `shown` provider = never stamped (fail closed).
  using KeyFn = bool (*)(uint32_t key);
  using LookFn = int (*)();   // the published ARENA look: 0 unknown, 1 match, 2 entrance (the fallback when the record has no stage look)
  void SetScreenGate(KeyFn shown, KeyFn bound, LookFn look = nullptr) { shown_ = shown, bound_ = bound, look_ = look; }
  // S103 E7: the game plays no movie (screen_planes_core.h PlaneStore::Idle: no display-set flush for kIdleFlushFrames). In the MATCH look (known, no titantron on the tron surfaces) a shown screen without a row
  // is then complete without one: there is no picture to wait for (run 26: 3949 match frames, 0 flushes, every shown screen "missing"). Never in the entrance look or an unknown look (a movie about to start must
  // keep the frame the game's). Read at the latch (Take), used at Emit.
  using IdleFn = bool (*)(ReadFn read);   // (read at the latch: the bound channels' state, review B4)
  void SetScreenIdle(IdleFn f) { idle_ = f; }
  void PlantNoIdle(bool on) { plant_no_idle_ = on; }   // unit only: the idle rule off (the pre-E7 gate)
  void PlantStampAlways(bool on) { plant_stamp_always_ = on; }   // unit only: stamp whatever the screens look like
  void PlantRequire800(bool on) { plant_require_800_ = on; }   // unit only: the gate wrongly requires the front end's own screen 800 in the match look
  // CStage::MotionSetup exit (arena_hooks.h): the stage object. Atomic: written on a game thread, read at the latch.
  // The arena the stage was built for is remembered with it (StageArena): the stage's OWN id, [stage + 1216] (arena_id_core.h kStageArenaIdOffset; the constructor sub_824E55B0 stores it), when it reads;
  // else the published arena at that moment. The stage's lifetime is the GAME's: the stage destructor sub_824E9858 (pre-hook, arena_hooks.h -> OnStageFreed) clears it the moment the game frees it. A freed
  // block keeps its old +1216 and the destructor writes no global, so no value-based guard can see a free: GuardLocked is only the second line (the stage's own id changed under it = the block was reused;
  // no readable own id = the old rule, a different published arena than the one it was set under).
  int32_t StageOwnId(uint32_t stage) const {
    uint32_t id = 0;
    return (stage && read_ && ReadWord(read_, stage + arena::kStageArenaIdOffset, id) && id < arena::kMaxArenaId) ? int32_t(id) : -1;
  }
  // The stage's raw id word for the arena hook (any value, readable or not); false = no stage / unreadable.
  bool StageIdRead(uint32_t& id) const {
    const uint32_t stage = Stage();
    return stage != 0 && read_ && ReadWord(read_, stage + arena::kStageArenaIdOffset, id);
  }
  bool StageLive() const { return Stage() != 0; }
  uint64_t CleanTakes() const { return clean_takes_.load(std::memory_order_relaxed); }
  void SetStage(uint32_t stage) {
    const int32_t own = StageOwnId(stage);
    stage_arena_.store(own >= 0 ? own : (arena_fn_ ? arena_fn_() : -1), std::memory_order_relaxed);
    if (stage_.load(std::memory_order_relaxed) != stage) {
      stage_gen_.fetch_add(1, std::memory_order_relaxed);   // S103 WSL (BH2 G5): a new stage object = a new generation, bumped BEFORE the stage changes (a take reads the stage, then the generation: it never pairs the new stage with the old generation)
    }
    if (stage_.exchange(stage, std::memory_order_relaxed) != stage) {
      last_show_.store(0, std::memory_order_relaxed);   // S103 SCF: the last bits belong to the old stage
      if (changed_) changed_();
    }
  }
  // The game frees stage `freed` (pre-hook on the destructor sub_824E9858, which the deleting wrapper sub_824EBE58 and the derived destructors all run): the current stage goes at once. Only the CURRENT stage: another
  // object (a stage built and freed without MotionSetup) leaves it alone. Plant (unit only): the hook does nothing.
  void PlantNoFreeHook(bool on) { plant_no_free_hook_ = on; }
  void OnStageFreed(uint32_t freed) {
    if (plant_no_free_hook_ || !freed) return;
    uint32_t cur = freed;
    if (!stage_.compare_exchange_strong(cur, 0u, std::memory_order_relaxed)) return;
    stage_arena_.store(-1, std::memory_order_relaxed);
    stage_gen_.fetch_add(1, std::memory_order_relaxed);   // S103 WSL
    last_show_.store(0, std::memory_order_relaxed);   // S103 SCF
    freed_.fetch_add(1, std::memory_order_relaxed);
    if (changed_) changed_();
  }
  uint64_t Freed() const { return freed_.load(std::memory_order_relaxed); }
  uint32_t Stage() const { return stage_.load(std::memory_order_relaxed); }
  uint32_t StageGen() const { return stage_gen_.load(std::memory_order_relaxed); }   // S103 WSL (unit)
  // S103 CAR: the pre-hook on sub_8342A8D8 (arena_hooks.h): the light index (r4 & 255) the loader asks /CARL/%04d for. The stage keeps no copy of it, so it is the one part index read from a hook rather than from the stage.
  // It is the LAST load's: the stage whose parts the loader just opened is the one built next (sub_8342A598 -> sub_8342AA30 -> the stage); never cleared by a free (a free may run after the next load began).
  void NoteCarLight(uint32_t r4) { car_light_.store(int32_t(r4 & 255u), std::memory_order_relaxed); }

  bool Latch(const FrameInfo& fi) override {
    if (!read_) return false;
    if (scene_frame::CurrentMode() == scene_frame::Mode::Off) {
      latched_.Clear();
      latched_.disabled = true;
    } else {
      Take(fi.guest_frame, latched_);
      // S111 CUTREACH2 (docs\research\s111\CUTREACH2.md, and its Review): everything this take reads that the walk DRAWS with is MODEL-class (written by update tasks before the walk, sub_82C302A0 +120 then
      // +1AC; the walk's draws land in the next stream), so record r carries latch r - 1's set, as ONE value: the slot worlds and posed palettes (the slot models' own draw), the vis rows (the look gate
      // sub_824E7190 runs in the group draw pass sub_825B9970 and writes nothing), the show bits (the screens' draw sub_8276AEF8 and that gate read them), the CAR indices (keyed on the vis stage / arena) and
      // the part clocks (c186: written by the model update sub_82BD5A98 from update tasks, read by the colour pass; E7 measured the stream's c186 = the latch before the record's, lag_finder.py). The appliers
      // join them (stage_posed.h: the posed rows only under the marker of their arena), so they are never split across two latches. The latch's own read at the boundary (the first latch, a skipped one,
      // after mode Off). The unreadable count follows the rows it counts (Emit's c.unknown, as the inputs contributor's). The screens (our movie feed), the idle flag and the stamps (decision health, on the hook
      // clock) stay THIS latch's; Take's CAR publish stays the newest (a load hint).
      {
        ModelClass now;
        now.slot_rows = std::move(latched_.slot_rows), now.vis = std::move(latched_.vis), now.show = latched_.show, now.car_present = latched_.car_present, now.car_ok = latched_.car_ok, now.car = latched_.car,
        now.clocks = std::move(latched_.clocks), now.unreadable = latched_.unreadable;
        ModelClass rec = model_period_.Pick(fi.guest_frame, std::move(now));
        latched_.slot_rows = std::move(rec.slot_rows), latched_.vis = std::move(rec.vis), latched_.show = rec.show, latched_.car_present = rec.car_present, latched_.car_ok = rec.car_ok, latched_.car = rec.car,
        latched_.clocks = std::move(rec.clocks), latched_.unreadable = rec.unreadable;
      }
      // S103 L1C review B1: bit 17 is stamped HERE, at the latch (the record's stamp snapshot is taken at the end of the latch, before Emit). Latch call only: the Emit fallback never stamps it.
      if constexpr (stageclock::kStampStageClock) {
        if (latched_.clocks_clean) scene_inputs::Capture(scene_inputs::kInStageClock, scene_inputs::HookFrameOfRecord(fi.guest_frame));
      }
    }
    mark_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    Frame scratch;
    const Frame* f = &latched_;
    if (!mark_.Use(fi.guest_frame, Name())) {   // not latched for this frame
      if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
      if (InDeferredEmit() || !read_) { ++c.stale; return; }   // a deferred emit must not read guest memory
      Take(fi.guest_frame, scratch);
      f = &scratch;
    }
    if (f->disabled) return;
    const size_t first_row = t.stage_fx.size();
    t.stage_fx.insert(t.stage_fx.end(), f->slot_rows.begin(), f->slot_rows.end());
    VisToRows(f->vis, t.stage_fx);     // S103 WSL: the marker (stage generation) + the slots the game's look gate skips
    if (f->car_present) CarToRows(f->car, f->car_ok, f->vis.stage, f->vis.arena, t.stage_fx);   // S103 CAR: a created arena's part indices
    ShowToRows(f->show, t.stage_fx);   // S103 SCF: 14 kFxShow rows (none when the bits did not read: the applier keeps the last ones)
    c.objects += uint32_t(f->slot_rows.size());
    t.part_clocks.insert(t.part_clocks.end(), f->clocks.begin(), f->clocks.end());   // S103 E5: the UV-scroll clock (c186) per stage model, read at the latch (part_clock_read.h)
    {   // the pairing + A1 check: each row against the kScArenaWorlds entry of the same model address (only when the arena-worlds contributor ran before this one: it is registered first)
      std::lock_guard<std::mutex> lk(cmu_);
      CrossCheckArenaWorlds(t.stage_fx, first_row, t.arena_worlds, census_);
    }
    c.unknown += f->unreadable;
    for (const ScreenSource& s : f->screens) {
      const uint32_t mi = MovieOfKey(t, s.key);
      if (mi == kNoMovie) {
        ++c.unknown;
        screens_no_movie_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      StageFxRow r;
      r.kind = kFxScreen;
      r.slot = s.key;
      r.movie = mi;
      r.seq = s.serial;
      r.w = s.w, r.h = s.h;
      t.stage_fx.push_back(r);
      ++c.objects;
      screen_rows_.fetch_add(1, std::memory_order_relaxed);
    }
    {   // S103 M (B2)
      // WHICH screens the look shows: the SAME signal the live applier (rf_screens.cpp) and the front end use, stage_look::Entrance(t): an entrance = the entrance set 802 804 805 807 809, else the match set 800 801 805 806 808
      // when the published arena look says match; a look that is neither known fails CLOSED (every screen of both sets is required).
      const bool ent = rdr_scene::stage_look::Entrance(t);
      const int arena_look = look_ ? look_() : 0;
      const bool known_match = !ent && arena_look == 1;
      bool tron_live = false;   // a titantron is playing on the tron surfaces this frame (a row for 804 / 807)
      for (const ScreenSource& s : f->screens) tron_live = tron_live || s.key == 804 || s.key == 807;
      bool complete = shown_ != nullptr;
      for (uint32_t key = 800; key <= 809 && complete; ++key) {
        const bool in_e = key == 802 || key == 804 || key == 805 || key == 807 || key == 809, in_m = key == 800 || key == 801 || key == 805 || key == 806 || key == 808;
        // S103 SCF [C, SCM.md]: the screens the game SHOWS are the ones whose stage record has bit 0x80 of byte +9 (803 too: a shown key draws, movie or grab). Bits unread this frame = the fixed sets by look (fail closed: every screen of both sets in an unknown look).
        if (f->show.known ? !f->show.KeyShown(int(key)) : !(ent ? in_e : known_match ? in_m : (in_e || in_m))) continue;   // the game does not show this screen
        // 800 is the front end's own in the MATCH look (front_end.cpp:1363: the previous frame's scene = the live camera feed), so it is not waited for there, and only while no titantron is playing (Jarrod, S97: the main
        // rectangle carries the superstar's tron and, in its absence, the camera feed); in an unknown look it stays required.
        if (key == 800 && known_match && !tron_live && !plant_require_800_) continue;
        if (MovieOfKey(t, uint16_t(key)) == kNoMovie || !shown_(key)) continue;
        if (key == 800 && known_match && tron_live) screens_800_tron_.fetch_add(1, std::memory_order_relaxed);   // S103 M (N4): 800 is required in this frame ONLY because a tron is playing
        bool row = false;
        for (const ScreenSource& s : f->screens) row = row || s.key == key;
        if (row) continue;
        if (known_match && !tron_live && f->screens_idle) {   // S103 E7: no movie plays: nothing to wait for (the screen keeps the arena file's own picture)
          screens_idle_.fetch_add(1, std::memory_order_relaxed);
          StageFxRow ir;   // review B3: tell the applier this screen shows the arena file's own picture now (it drops a live texture a movie left behind)
          ir.kind = kFxScreen, ir.slot = uint16_t(key), ir.movie = MovieOfKey(t, uint16_t(key)), ir.w = 0, ir.h = 0, ir.flags = kFxfScreenIdle;
          t.stage_fx.push_back(ir);
          continue;
        }
        complete = false;
        screens_missing_.fetch_add(1, std::memory_order_relaxed);
        if (bound_ && !bound_(key)) screens_unbound_movie_.fetch_add(1, std::memory_order_relaxed);
      }
      if (complete || plant_stamp_always_) {
        scene_inputs::Capture(scene_inputs::kInStageScreens, scene_inputs::HookFrameOfRecord(fi.guest_frame));
        screens_complete_.fetch_add(1, std::memory_order_relaxed);
      } else {
        screens_incomplete_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  void Census(std::string& out) override {
    const SlotCensus cn = SlotStats();
    out += "[stage_fx frames " + std::to_string((unsigned long long)cn.frames) + ", slot_rows " + std::to_string((unsigned long long)cn.rows) + ", no_model " + std::to_string((unsigned long long)cn.no_model) +
           ", no_motion " + std::to_string((unsigned long long)cn.no_motion) + ", unreadable " + std::to_string((unsigned long long)cn.unreadable) + ", no_stage " + std::to_string((unsigned long long)cn.no_stage) +
           ", palette_unread " + std::to_string((unsigned long long)cn.palette_unread) + ", node_assumed " + std::to_string((unsigned long long)cn.node_assumed) + ", palette_differ " + std::to_string((unsigned long long)cn.differ) +
           ", worlds_join equal " + std::to_string((unsigned long long)cn.join_equal) + " differ " + std::to_string((unsigned long long)cn.join_differ) + " none " + std::to_string((unsigned long long)cn.join_none) + ", screen_rows " +
           std::to_string((unsigned long long)screen_rows_.load(std::memory_order_relaxed)) + ", screens_unbound " + std::to_string((unsigned long long)screens_unbound_.load(std::memory_order_relaxed)) + ", screens_no_movie " +
           std::to_string((unsigned long long)screens_no_movie_.load(std::memory_order_relaxed)) + ", screens_complete " + std::to_string((unsigned long long)screens_complete_.load(std::memory_order_relaxed)) +
           " incomplete " + std::to_string((unsigned long long)screens_incomplete_.load(std::memory_order_relaxed)) + " shown_without_row " + std::to_string((unsigned long long)screens_missing_.load(std::memory_order_relaxed)) +
           " of_which_unbound " + std::to_string((unsigned long long)screens_unbound_movie_.load(std::memory_order_relaxed)) + " s800_required_tron " + std::to_string((unsigned long long)screens_800_tron_.load(std::memory_order_relaxed)) + " idle_no_movie " + std::to_string((unsigned long long)screens_idle_.load(std::memory_order_relaxed)) + "]";   // S103 E7
    {   // S103 WSL: the census line of the game's slot look gate: how many slots it skipped in the last read take, takes read / unread
      char buf[256];
      std::snprintf(buf, sizeof buf, "[slot_look skipped %u, takes %llu, unread %llu, stage_gen %u, cycle_groups %u pick %u tick %u]", vis_skipped_.load(std::memory_order_relaxed), (unsigned long long)vis_ok_.load(std::memory_order_relaxed), (unsigned long long)vis_unread_.load(std::memory_order_relaxed), stage_gen_.load(std::memory_order_relaxed), cyc_groups_.load(std::memory_order_relaxed), cyc_pick_.load(std::memory_order_relaxed), cyc_tick_.load(std::memory_order_relaxed));   // S103 CAR: cycle groups read in the last take, the first group's picked slot and tick
      out += buf;
      std::snprintf(buf, sizeof buf, "[posed_slots %u, cap_dropped %llu]", posed_slots_.load(std::memory_order_relaxed), (unsigned long long)posed_dropped_.load(std::memory_order_relaxed));   // S110 BARRICADE: palette-posed slots written in the last take; slots the row cap left out (total)
      out += buf;
    }
    {   // S103 CAR: the created-arena census: the part indices read from the game, the loads made (parts, bands)
      const uint64_t l = car_last_.load(std::memory_order_relaxed);
      const assets::car::Counters& cs = assets::car::Stats();
      char buf[384];
      std::snprintf(buf, sizeof buf, "[created_arena takes_read %llu unread %llu, last caen %u casd %u carl %u bg %d, loads %llu failed %llu, missing_part %llu fallback_index %llu, parts caen %u casd %u carl %u, slots casd %u caen %u carg %u, entries %u]",
                    (unsigned long long)car_ok_.load(std::memory_order_relaxed), (unsigned long long)car_unread_.load(std::memory_order_relaxed), unsigned(l & 0xFFFF), unsigned(l >> 16 & 0xFFFF), unsigned(l >> 32 & 0xFFFF), int(int16_t(l >> 48)),
                    (unsigned long long)cs.builds.load(std::memory_order_relaxed), (unsigned long long)cs.build_failed.load(std::memory_order_relaxed), (unsigned long long)cs.missing_part.load(std::memory_order_relaxed), (unsigned long long)cs.fallback_index.load(std::memory_order_relaxed),
                    cs.last_caen.load(std::memory_order_relaxed), cs.last_casd.load(std::memory_order_relaxed), cs.last_carl.load(std::memory_order_relaxed), cs.last_slots_casd.load(std::memory_order_relaxed), cs.last_slots_caen.load(std::memory_order_relaxed), cs.last_slots_carg.load(std::memory_order_relaxed), cs.last_entries.load(std::memory_order_relaxed));
      out += buf;
    }
    {   // S103 SCF: the game's show bits on the census line, so the step check reads which branch a match took: art = slots 301-304, keys = 800-809 (1 = shown, ? = never read)
      const uint32_t b = last_show_.load(std::memory_order_relaxed);
      char buf[256];
      const auto bit = [&](int i) { return (b >> 31) ? ((b >> i & 1u) ? '1' : '0') : '?'; };
      std::snprintf(buf, sizeof buf, "[stage_show art 301:%c 302:%c 303:%c 304:%c, keys 800:%c 801:%c 802:%c 803:%c 804:%c 805:%c 806:%c 807:%c 808:%c 809:%c, unread %llu]", bit(16), bit(17), bit(18), bit(19), bit(0), bit(1), bit(2), bit(3), bit(4), bit(5), bit(6), bit(7), bit(8), bit(9), (unsigned long long)show_unread_.load(std::memory_order_relaxed));
      out += buf;
    }
    {   // S103 E5
      std::lock_guard<std::mutex> lk(cmu_);
      out += "[part_clocks models " + std::to_string((unsigned long long)clock_census_.models) + ", rows " + std::to_string((unsigned long long)clock_census_.rows) + ", no_parts " + std::to_string((unsigned long long)clock_census_.no_parts) +
             ", unreadable " + std::to_string((unsigned long long)clock_census_.unreadable) + ", absurd " + std::to_string((unsigned long long)clock_census_.absurd) + ", non_finite " + std::to_string((unsigned long long)clock_census_.non_finite) +
             ", duplicates " + std::to_string((unsigned long long)clock_census_.duplicates) + ", overflow " + std::to_string((unsigned long long)clock_census_.overflow) + "]";
    }
    out += "[" + model_period_.CensusLine("stage_fx_period") + "]";   // S111 CUTREACH2 review: records that took the slot / vis / show / CAR / clock set of the latch before theirs (latch_period.h)
    if (extra_census_) extra_census_(out);   // S103 M
  }

  struct Frame {
    bool disabled = false;
    std::vector<StageFxRow> slot_rows;
    std::vector<ScreenSource> screens;
    std::vector<PartClock> clocks;   // S103 E5
    bool screens_idle = false;       // S103 E7: no movie plays (read at the latch)
    bool clocks_clean = false;       // S103 E5: every stage model's clocks were read in this take (a clean stage, rows, nothing refused)
    uint32_t unreadable = 0;
    StageShow show;                  // S103 SCF: the show bits of keys 800-809 and art slots 301-304, read at the latch; known = false when the stage / the records did not read (the applier keeps the last bits)
    bool car_present = false, car_ok = false;   // S103 CAR: the stage is a created arena's (200-249); every part index was read
    assets::car::Sel car;            // S103 CAR: CAEN / CASD ([stage + 21184 / 21188]), CARL (the sub_8342A8D8 pre-hook), the real bg id ([stage + 21192])
    StageVis vis;                    // S103 WSL: the game's slot look gate (stage_slot_vis.h), read at the latch; known = false when a word / the records did not read (the applier keeps the last mask of the same generation)
    void Clear() { disabled = false, slot_rows.clear(), screens.clear(), clocks.clear(), clocks_clean = false, screens_idle = false, unreadable = 0, show = StageShow(), vis.marker = false, vis.known = false, vis.gen = vis.stage = 0, vis.arena = kVisArenaUnread, vis.skipped.clear(), car_present = car_ok = false, car = assets::car::Sel(); }
  };
  // S111 CUTREACH2 review: the part of a take the walk draws with (Latch: record r carries latch r - 1's, latch_period.h).
  struct ModelClass {
    std::vector<StageFxRow> slot_rows;
    StageVis vis;
    StageShow show;
    bool car_present = false, car_ok = false;
    assets::car::Sel car;
    std::vector<PartClock> clocks;
    uint32_t unreadable = 0;   // the slots of that take whose rows could not be read (Emit's c.unknown)
  };
  // One frame's take (public for the unit).
  void Take(uint64_t frame, Frame& f) {
    wwe2k14::safe_pages::PassLines pass_lines;   // S108 RDS review: this pass's line copy (the game's ReadGuest serves this thread's reads of up to 128 bytes through it until return: BuildSlotRows / ReadSlotWorld / ReadMat, ReadStageClocks' ReadModelClocks, the show / vis / CAR / paint words)
    f.Clear();
    if (moving_) moving_(moving_buf_);
    else moving_buf_.clear();
    {
      std::lock_guard<std::mutex> lk(cmu_);
      GuardLocked();
      const uint64_t before_unreadable = census_.unreadable, before_no_motion = census_.no_motion;
      BuildSlotRows(read_, Stage(), moving_buf_, uint32_t(frame), f.slot_rows, census_);
        {   // S110 BARRICADE [C, docs\research\s110\BARRICADE.md]: every slot whose model has more than one node, its palette ([model + 200]) as kFxSlotPalette rows (same take)
          const ReadFn rd = read_;
          std::vector<PosedSlot> posed;
          if (ReadPosedSlots([rd](uint32_t a, void* d, size_t n) { return rd(a, d, n); }, Stage(), posed)) {
            PosedWithoutWorldRows(posed, f.slot_rows);   // exactly one of kFxSlotWorld / kFxSlotPalette per slot
            const size_t dropped = PosedToRows(posed, uint32_t(frame), f.slot_rows);
            posed_slots_.store(uint32_t(posed.size() - dropped), std::memory_order_relaxed);
            if (dropped) posed_dropped_.fetch_add(dropped, std::memory_order_relaxed);   // S110 review: a hit row cap is counted (census), never silent
          }
        }
      {   // S103 SCF [C, SCM.md]: the game's show bits (stage + 6008 key records, stage + 1220 + 12 * 301 art-slot records, byte +9 bit 0x80). Unreadable = no rows this frame: the applier keeps the LAST bits (never all-on, never all-off)
        const ReadFn rd = read_;
        if (!ReadStageShow([rd](uint32_t a, void* d, size_t n) { return rd(a, d, n); }, Stage(), f.show)) {
          if (Stage() != 0) show_unread_.fetch_add(1, std::memory_order_relaxed);
          const uint32_t lk = last_show_.load(std::memory_order_relaxed);   // fail closed: keep the LAST bits read from this stage (legacy sets only if never read)
          if (lk >> 31) f.show.known = true, f.show.keys = uint16_t(lk & 0x3FFu), f.show.art = uint8_t(lk >> 16 & 0xFu);
        } else {
          last_show_.store(uint32_t(f.show.keys) | uint32_t(f.show.art) << 16 | 1u << 31, std::memory_order_relaxed);
        }
      }
      {   // S103 WSL (COV6 G5) [C listing sub_824E7190 0x824E72A8-0x824E7500]: the slots the game's look gate skips this frame. Any unread word / record = no skip rows (marker only): the applier keeps the last mask
        const ReadFn rd = read_;
        const uint32_t stg = Stage();                                       // once per take: the stage first, then its generation (SetStage bumps the generation before it swaps the stage)
        f.vis.stage = stg;
        f.vis.gen = stage_gen_.load(std::memory_order_relaxed);
        f.vis.marker = stg != 0;
        uint32_t arena_word = 0;
        if (f.vis.marker && ReadWord(rd, stg + kStageOffArenaId, arena_word)) f.vis.arena = arena_word;
        SlotVisWords vw;
        uint8_t recs[kSlotVisSlots * kStageRecordSize];
        std::vector<CycleGroup> cyc;
        CycleWords cw;
        if (f.vis.marker && ReadVisWords([rd](uint32_t a, void* d, size_t n) { return rd(a, d, n); }, stg, vw) && rd(stg + kStageSlotRecordsAt, recs, sizeof recs) &&
            ReadCycleGroups([rd](uint32_t a, void* d, size_t n) { return rd(a, d, n); }, stg, cyc, cw)) {
          BuildVisSkips(recs, vw, f.vis.skipped);
          CycleSkips(cyc, cw, recs, f.vis.skipped);   // S103 CAR (BCA G2): the game draws one slot of each cycle group; the others are skipped (sort + unique: StageVis::operator== compares the vector)
          cyc_groups_.store(uint32_t(cyc.size()), std::memory_order_relaxed);
          if (!cyc.empty()) cyc_pick_.store(cyc[0].slot[CyclePick(cyc[0])], std::memory_order_relaxed), cyc_tick_.store(cyc[0].tick, std::memory_order_relaxed);
          f.vis.known = true;
          vis_skipped_.store(uint32_t(f.vis.skipped.size()), std::memory_order_relaxed);
          vis_ok_.fetch_add(1, std::memory_order_relaxed);
        } else if (f.vis.marker) {
          vis_unread_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (f.vis.marker && f.vis.arena != kVisArenaUnread && VisArena200to249(f.vis.arena)) {   // S103 CAR (BCA G1): a created arena's stage: its part indices, read with the stage (one row beside the WSL marker)
        const ReadFn rd = read_;
        f.car_present = true;
        f.car_ok = ReadCarSel([rd](uint32_t a, void* d, size_t n) { return rd(a, d, n); }, f.vis.stage, f.vis.arena, car_light_.load(std::memory_order_relaxed), f.car);
        (f.car_ok ? car_ok_ : car_unread_).fetch_add(1, std::memory_order_relaxed);
        if (f.car_ok) assets::car::Publish(f.car);   // S103 CAR fix (blocker): published at the latch, so the slot provider and the crowd / ring readers (arena_hooks.h, OpenCached) see the venue before the record names the arena
        if (f.car_ok) car_last_.store(uint64_t(f.car.caen) | uint64_t(f.car.casd) << 16 | uint64_t(f.car.carl) << 32 | uint64_t(uint16_t(f.car.bg)) << 48, std::memory_order_relaxed);
        carpaint::OnLatch(f.vis.stage, read_, f.vis.gen);   // S108 CAP: the stage's own texture list [stage + 14452] as the game painted it (car_paint.h), read here with the game threads parked; the generation tells a new stage at a reused address
      }
      f.unreadable = uint32_t(census_.unreadable - before_unreadable);
      const bool slots_ready = slots_ready_ ? slots_ready_() : true;
      const bool clean = Stage() != 0 && f.unreadable == 0 && census_.no_motion == before_no_motion && slots_ready;
      clean_stage_.store(clean ? Stage() : 0u, std::memory_order_relaxed);   // S103 A3 (review F2)
      if (clean) clean_takes_.fetch_add(1, std::memory_order_relaxed);   // S103 CAE: the census of a clean take between G+64 changing and the new stage (arena_id_core.h)
      if (clean && stamp_) stamp_(frame);   // same frame (audit item 1)
      const ClockCensus cb = clock_census_;   // S103 E5
      ReadStageClocks(read_, Stage(), uint32_t(scene_inputs::HookFrameOfRecord(frame)), f.clocks, clock_census_, clock_scratch_);   // PartClock::frame = the HOOK frame (the record's minus its lag)
      f.clocks_clean = clean && clock_census_.unreadable == cb.unreadable && clock_census_.absurd == cb.absurd && clock_census_.non_finite == cb.non_finite && clock_census_.overflow == cb.overflow;
    }
    f.screens_idle = idle_ && !plant_no_idle_ && idle_(read_);   // S103 E7
    if (screens_) {
      ScreenSource buf[16];
      const size_t n = screens_(buf, 16);
      for (size_t i = 0; i < n && i < 16; ++i)
        if (buf[i].key >= 800 && buf[i].key <= 809 && buf[i].w && buf[i].h) f.screens.push_back(buf[i]);
    } else {
      screens_unbound_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // The arena id the current stage object was set under: its own id (+1216) when it read, else the published arena at SetStage (S103 A7; CAE). -1 = no stage / unknown. Not the live published arena: compare
  // the two to know the published arena is this stage's (stage_fx_glue.h slots_ready).
  int32_t StageArena() const { return stage_arena_.load(std::memory_order_relaxed); }   // S103 A7: the arena id the current stage object was set under (-1 = unknown)
  // S103 V1 (R17 [C], sub_824E7190 0x824E71C8-0x824E71F0): the stage object's slot records (399 x 12 bytes at stage + 1220), read at the latch for the arena contributor's "does the game draw this slot" bit.
  // false = no current stage / unreadable.
  // Reads through CurrentStage (below): the stage only while the game has not freed it (OnStageFreed) and its own id still matches (GuardLocked).
  // S103 SCF census: the last bits read (keys 800-809 in the low 10 bits, art slots 301-304 in bits 16-19, bit 31 = ever read), and how many takes could not read them.
  uint32_t LastShow() const { return last_show_.load(std::memory_order_relaxed); }
  uint64_t ShowUnread() const { return show_unread_.load(std::memory_order_relaxed); }
  bool ReadSlotRecords(uint8_t* out, size_t n) {
    uint32_t stage = 0;
    if (!read_ || !CurrentStage(stage)) return false;
    return read_(stage + 1220u, out, n);
  }
  // S103 A3 (review F2): the stage object IF it is still live: runs the guard (GuardLocked), so a block the game has reused is never read. Called by the inputs contributor before it reads anything through the stage.
  bool CurrentStage(uint32_t& stage) {
    {
      std::lock_guard<std::mutex> lk(cmu_);
      GuardLocked();
    }
    stage = Stage();
    return stage != 0;
  }
  // S103 A3 (audit item 1): the arena_worlds input is stamped HERE, from the same take that judged the stage clean (the inputs contributor latches before this one, so its own view of "clean" is the
  // previous frame's). `stamp` gets the record's frame; the glue turns it into the hook-clock stamp. Not called when the take is not clean.
  using StampFn = void (*)(uint64_t record_frame);
  void SetStamp(StampFn f) { stamp_ = f; }
  using ChangedFn = void (*)();   // S103 A3 (F2): the stage object changed or the arena guard dropped it (the per-node light table belongs to the old scene)
  void SetStageChanged(ChangedFn f) { changed_ = f; }
  using SlotsReadyFn = bool (*)();   // the arena's slot table is published for the current arena (arena_contrib.h: !NeedsSlots)
  void SetSlotsReady(SlotsReadyFn f) { slots_ready_ = f; }
  // S103 A3 (A1-A4): every arena slot's model object = the arena object list for the latch readers (MdlAlpha, the override tables). Source [C stage_fx_core.h, sub_824E5340 listing]: the word at
  // stage + 6164 + 4 * slot, slots 0..398, 0 = no model. No learn table and no new hook: the stage object is the one the existing MotionSetup post-hook stored. One read of the whole array.
  // Returns false (and clears `out`) when there is no stage or the array is unreadable.
  bool StageModels(std::vector<uint32_t>& out) {
    out.clear();
    uint32_t stage = 0;
    if (!read_ || !CurrentStage(stage)) return false;
    uint32_t raw[kSlotCount];
    if (!read_(stage + kStageModels, raw, sizeof raw)) return false;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
      const uint32_t m = Swap32(raw[i]);
      if (m < 0x10000) continue;
      bool dup = false;   // the same model object in two slots would give double rows (review note): the first slot keeps it
      for (uint32_t k : out) dup = dup || k == m;
      if (!dup) out.push_back(m);
    }
    return true;
  }
  // S103 A5: StageModels with each model's stage slot (parallel arrays, the same dedup: the first slot keeps a model). false = no stage / unreadable (both cleared).
  bool StageModelsWithSlots(std::vector<uint32_t>& out, std::vector<uint16_t>& slots) {
    out.clear();
    slots.clear();
    uint32_t stage = 0;
    if (!read_ || !CurrentStage(stage)) return false;
    uint32_t raw[kSlotCount];
    if (!read_(stage + kStageModels, raw, sizeof raw)) return false;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
      const uint32_t m = Swap32(raw[i]);
      if (m < 0x10000) continue;
      bool dup = false;
      for (uint32_t k : out) dup = dup || k == m;
      if (!dup) out.push_back(m), slots.push_back(uint16_t(i));
    }
    return true;
  }
  // The last Take saw a stage and read every row it tried (the arena_worlds input's precondition on this side).
  // Clean = a take of THIS stage object found a stage, read every row it tried, saw no moving slot without a motion player, and had the slot table published for the current arena (review F2 3).
  // The stage is re-validated here, so a clean take of a stage the arena has since replaced counts for nothing.
  bool LastTakeClean() {
    uint32_t stage = 0;
    return CurrentStage(stage) && clean_stage_.load(std::memory_order_relaxed) == stage;
  }
  SlotCensus SlotStats() const {
    std::lock_guard<std::mutex> lk(cmu_);
    return census_;
  }

  static constexpr uint32_t kNoMovie = 0xFFFFFFFFu;
  // The record's Movie of stage screen `key`: the arena contributor writes one OnTarget Movie per screen whose Target is the TronPicture of index key - 800.
  static uint32_t MovieOfKey(const SceneTables& t, uint16_t key) {
    for (uint32_t i = 0; i < t.movies.size(); ++i) {
      const Movie& m = t.movies[i];
      if (m.show != uint8_t(MovieShow::OnTarget) || m.target >= t.targets.size()) continue;
      const Target& tg = t.targets[m.target];
      if (tg.role == rdr_adapter::SurfaceRole::TronPicture && tg.index == key - 800) return i;
    }
    return kNoMovie;
  }

 private:
  // cmu_ held. The arena guard (moved out of Take unchanged):
  void GuardLocked() {
    if (Stage() != 0) {   // S103 CAE: the stage's own id no longer the one it was set under: the block was reused (a free is caught earlier, by OnStageFreed). Gone.
      const int32_t own = StageOwnId(Stage()), sa0 = stage_arena_.load(std::memory_order_relaxed);
      if (own >= 0 && sa0 >= 0) {
        if (own != sa0) {
          stage_.store(0, std::memory_order_relaxed);
          if (changed_) changed_();
        }
        return;   // the stage names its own arena: the (lagging) published id is not asked
      }
    }
    if (arena_fn_ && Stage() != 0) {   // no readable own id: a different published arena than the stage was set under: that stage object is gone
        const int32_t a = arena_fn_(), sa = stage_arena_.load(std::memory_order_relaxed);
        if (a >= 0 && sa >= 0 && a != sa) {
          stage_.store(0, std::memory_order_relaxed);
          if (changed_) changed_();
        }
        else if (a >= 0 && sa < 0) stage_arena_.store(a, std::memory_order_relaxed);   // set before any arena was known: adopt the first one seen
      }
  }
  ReadFn read_ = nullptr;
  MovingFn moving_ = nullptr;
  ScreensFn screens_ = nullptr;
  CensusFn extra_census_ = nullptr;   // S103 M
  ArenaFn arena_fn_ = nullptr;
  std::atomic<uint32_t> stage_{0};
  std::atomic<int32_t> stage_arena_{-1};
  std::atomic<uint64_t> clean_takes_{0}, freed_{0};   // S103 CAE
  bool plant_no_free_hook_ = false;
  std::atomic<uint32_t> clean_stage_{0};   // S103 A3: the stage object of the last clean take (0 = none)
  SlotsReadyFn slots_ready_ = nullptr;
  StampFn stamp_ = nullptr;
  ChangedFn changed_ = nullptr;
  LatchMark mark_;
  Frame latched_;
  scene_inputs::period::OfPalette<ModelClass> model_period_;   // S111 CUTREACH2: record r takes latch r - 1's model-class set (latch thread only)
  std::vector<uint16_t> moving_buf_;   // reused (the latch / a non-deferred Emit: one thread at a time)
  mutable std::mutex cmu_;             // census_ (the census line is read from another thread)
  SlotCensus census_;
  ClockCensus clock_census_;
  ClockScratch clock_scratch_;   // S103 E5 (under cmu_): the readers' buffers, grown once   // S103 E5 (under cmu_)
  std::atomic<uint64_t> screen_rows_{0}, screens_unbound_{0}, screens_no_movie_{0};
  std::atomic<uint64_t> screens_complete_{0}, screens_incomplete_{0}, screens_missing_{0}, screens_unbound_movie_{0}, screens_800_tron_{0}, screens_idle_{0}, show_unread_{0};   // S103 M (B2, N4); S103 E7; S103 SCF
  std::atomic<int32_t> car_light_{-1};   // S103 CAR: the last CARL index the loader asked for (-1 = never)
  std::atomic<uint64_t> car_ok_{0}, car_unread_{0}, car_last_{0};   // S103 CAR census: takes of a created arena's stage whose indices read / did not, the last read (caen | casd << 16 | carl << 32 | bg << 48)
  std::atomic<uint32_t> stage_gen_{0};   // S103 WSL: bumped when the stage object changes or is freed (stage_slot_vis.h marker row)
  std::atomic<uint32_t> vis_skipped_{0}, cyc_groups_{0}, cyc_pick_{0}, cyc_tick_{0};   // S103 CAR: the cycle census
  std::atomic<uint64_t> vis_ok_{0}, vis_unread_{0};
  std::atomic<uint32_t> posed_slots_{0};     // S110 BARRICADE: posed slots written in the last take
  std::atomic<uint64_t> posed_dropped_{0};   // S110 review: posed slots the row cap left out (total)
  std::atomic<uint32_t> last_show_{0};   // S103 SCF: the last show bits read (LastShow)
  KeyFn shown_ = nullptr, bound_ = nullptr;
  IdleFn idle_ = nullptr;   // S103 E7
  bool plant_no_idle_ = false;
  LookFn look_ = nullptr;
  bool plant_stamp_always_ = false, plant_require_800_ = false;
};

inline StageFxContributor& StageFx() {
  static StageFxContributor s;
  return s;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterStageFx() {   // idempotent; registered AFTER the arena contributor (Emit order: its Movies / Targets are in the record first)
  if (!RegisteredFlag().exchange(true)) Register(&StageFx());
}
inline bool StageFxRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::stagefx
