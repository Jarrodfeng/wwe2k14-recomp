// The crowd camera flashes as a contributor to the scene record (S103 lane C3; docs\renderer\STAGE_LIVE_WRITERS.md section 4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes (the glue gives it the guest reader). Writes one kFxFlash row per live flash (flash_fx_core.h: where the items are, the liveness rule, every field's source) into
// the StageFxRow table (record section kScStageFx, id 39) that lane C's ApplyFlashes reads. Read only, at the frame's LATCH (game threads parked): the items are written by the update
// (spawn / counter) and by the flash draw (visibility), and the draw is NOT skipped under OURS [C: no hook names sub_823B38C0 / sub_823B27F0], so the state at the latch is the walk's.
// Registered AFTER the stage-fx contributor (its slot-world rows come first; the order of rows does not matter to the appliers). Off by default and free when off: with
// --wwe2k14_rdr_scene = 0 the latch reads nothing and Emit writes nothing.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../renderer_game/scene_frame.h"
#include "../renderer_scene/rows_stage_fx.h"   // S106 G4-2
#include "../renderer_scene/live_scene.h"
#include "flash_fx_core.h"
#include "flash_hold_core.h"   // S110 FLASHHOLD
#include "tail_latch.h"   // LatchMark

namespace rdr_scene::live::flashfx {

class FlashFxContributor final : public Contributor {
 public:
  const char* Name() const override { return "flash_fx"; }
  void SetReader(ReadFn r) { read_ = r; }
  void SetHoldFn(uint32_t (*f)()) { hold_fn_ = f; }   // S110 FLASHHOLD: k, the guest frames each game row is shown (FLASH60)

  bool Latch(const FrameInfo& fi) override {
    if (!read_) return false;
    latched_.clear();
    if (scene_frame::CurrentMode() != scene_frame::Mode::Off) Take(fi.guest_frame, latched_);
    mark_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    std::vector<StageFxRow> scratch;
    const std::vector<StageFxRow>* rows = &latched_;
    if (!mark_.Use(fi.guest_frame, Name())) {   // not latched for this frame
      if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
      if (InDeferredEmit() || !read_) { ++c.stale; return; }   // a deferred emit must not read guest memory
      Take(fi.guest_frame, scratch);
      rows = &scratch;
    }
    t.stage_fx.insert(t.stage_fx.end(), rows->begin(), rows->end());
    c.objects += uint32_t(rows->size());
  }
  void Census(std::string& out) override {
    const FlashCensus c = Stats();
    auto n = [](uint64_t v) { return std::to_string((unsigned long long)v); };
    out += "[flash_fx frames " + n(c.frames) + ", rows " + n(c.rows) + " (last_frame " + n(c.last_frame) + "), no_manager " + n(c.no_manager) + ", no_pass " + n(c.no_pass) + ", no_loop " + n(c.no_loop) + ", empty " + n(c.empty_slots) +
           ", not_drawn " + n(c.not_drawn) + ", pending " + n(c.pending) + ", invisible " + n(c.invisible) + ", done " + n(c.done) + ", not_item " + n(c.not_item) + ", bad_values " + n(c.bad_values) + ", unspawned " + n(c.unspawned) + ", unreadable " + n(c.unreadable) + ", held " + n(c.held) +
           ", light_set_frames " + n(c.light_set_frames) + ", color_unread " + n(c.color_unread) + "]";
  }
  // One frame's take (public for the unit).
  void Take(uint64_t frame, std::vector<StageFxRow>& rows) {
    std::lock_guard<std::mutex> lk(cmu_);
    // The game's 30 fps scene shows each flash step for one 33 ms tick; our 60 fps scene setting would show it 16.7 ms: hold k guest frames (S110 FLASH60).
    std::vector<StageFxRow> game;
    BuildFlashRows(read_, uint32_t(frame), game, census_, state_);
    census_.held += HoldRows(game, frame, hold_fn_ ? hold_fn_() : 1u, hold_, rows);
  }
  FlashCensus Stats() const {
    std::lock_guard<std::mutex> lk(cmu_);
    return census_;
  }

 private:
  ReadFn read_ = nullptr;
  LatchMark mark_;
  std::vector<StageFxRow> latched_;
  mutable std::mutex cmu_;   // census_ (the census line is read from another thread)
  FlashCensus census_;
  FlashState state_;   // per slot: the item and whether the last latch saw it done (guarded by cmu_)
  HoldState hold_;     // S110 FLASHHOLD (guarded by cmu_)
  uint32_t (*hold_fn_)() = nullptr;
};

inline FlashFxContributor& FlashFx() {
  static FlashFxContributor s;
  return s;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterFlashFx() {   // idempotent
  if (!RegisteredFlag().exchange(true)) Register(&FlashFx());
}
inline bool FlashFxRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::flashfx
