// The wrestlers' liquid maps as a live contributor to the scene record (Session 103, lane SWF; docs\systems\liquid_map.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes. A Contributor named "liquid", registered once at startup (wwe2k14_app.h OnPostSetup, with --wwe2k14_scene_records and --wwe2k14_rdr_liquid_map, default on; until then the hook
// returns at its first check). Emit appends every closed hook frame it has not emitted yet, oldest first, as kScLiquidMaps / kScLiquidItems / kScLiquidPictures rows (scene_record.h): a frame with no live item and no
// clear request contributes nothing, so a dry match writes no liquid section at all. The rows carry the HOOK's guest frame (never the record's; the record's header runs one frame ahead of the hook's stamp, sphere_contrib.h).
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "liquid_hook.h"
#include "tail_latch.h"

namespace rdr_scene::live::liquid {

class LiquidContributor : public Contributor {
 public:
  const char* Name() const override { return "liquid"; }
  bool Latch(const FrameInfo& fi) override { return latch_.Take(tracker, fi); }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    (void)c;
    latch_.Begin(tracker, fi, Name());
    while (latch_.Next(tracker, scratch_)) {
      ++frames_emitted_;
      for (MapEntry& m : scratch_.maps) {
        m.row.item_first = uint32_t(t.liquid_items.size());
        t.liquid_items.insert(t.liquid_items.end(), m.items.begin(), m.items.end());
        t.liquid_maps.push_back(m.row);
        ++maps_emitted_, items_emitted_ += m.items.size();
      }
      for (const CrowdPictureRow& p : scratch_.pictures) {
        bool have = false;
        for (const CrowdPictureRow& q : t.liquid_pictures) have = have || q.key == p.key;
        if (!have) t.liquid_pictures.push_back(p);
      }
    }
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[liquid calls " + std::to_string(s.calls) + ", recorded " + std::to_string(s.recorded) + ", replaced " + std::to_string(s.replaced) + ", early_out " + std::to_string(s.early_out) + " (disabled " + std::to_string(s.disabled) +
           "), unreadable " + std::to_string(s.unreadable) + ", items " + std::to_string(s.items) + " (undrawn " + std::to_string(s.items_undrawn) + ", overflow " + std::to_string(s.items_overflow) + "), clears " + std::to_string(s.clears) +
           ", pictures read " + std::to_string(s.pictures_read) + " hit " + std::to_string(s.picture_hits) + ", tex_class " + std::to_string(s.tex_class) + " tex_null " + std::to_string(s.tex_null) + " tex_fail " + std::to_string(s.tex_fail) +
           ", no_object " + std::to_string(s.no_object) + ", late " + std::to_string(s.late) + ", overflow " + std::to_string(s.overflow) + ", frames " + std::to_string(s.frames) + ", emitted frames " + std::to_string(frames_emitted_) +
           " maps " + std::to_string(maps_emitted_) + " items " + std::to_string(items_emitted_) + "]";
  }
  Tracker tracker;
  const TrackerLatch<FrameBuffer, kRing>& Latched() const { return latch_; }   // (tests)

 private:
  TrackerLatch<FrameBuffer, kRing> latch_;
  FrameBuffer scratch_;
  uint64_t frames_emitted_ = 0, maps_emitted_ = 0, items_emitted_ = 0;
};

inline LiquidContributor& Liquid() {
  static LiquidContributor r;
  return r;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterLiquid() {   // idempotent; the hook does nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&Liquid());
}
inline bool LiquidRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::liquid
