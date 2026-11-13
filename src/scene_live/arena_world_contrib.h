// The arena's evaluated worlds as a live contributor to the scene record (Session 101, lane 130; docs\renderer\SCENE_RECORD.md section kScArenaWorlds).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game or SDK includes. A Contributor named "arena_worlds", registered once at startup
// (wwe2k14_app.h OnPostSetup, with --wwe2k14_scene_records; the model-entry hook returns at its first check until then). Emit appends every closed frame it has not
// emitted yet, oldest first: the kAwObject entries as the hook wrote them (each stamped with the HOOK's frame, never the record's) and one kAwClock entry per frame
// when the stage origin is known. The same guest point as the rest of the arena section: the model call on the drawing thread (the light-state tracker's point).
#pragma once
#include <atomic>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "arena_world_hook.h"

namespace rdr_scene::live::arenaworld {

class WorldContributor : public Contributor {
 public:
  const char* Name() const override { return "arena_worlds"; }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    tracker.CloseBefore(fi.guest_frame);   // lane 152 (B1)
    bool any = false;
    while (tracker.TakeNext(scratch_)) {
      any = true;
      t.arena_worlds.insert(t.arena_worlds.end(), scratch_.worlds.begin(), scratch_.worlds.end());
      // (c.objects is not touched: these entries are not scene Objects; the census line below counts them)
      ArenaWorld ck;
      ck.kind = kAwClock;
      ck.frame = uint32_t(scratch_.frame);
      uint32_t origin = 0;
      if (tracker.ClockFor(scratch_, ck.clock, origin)) {
        ck.origin = origin;
        t.arena_worlds.push_back(ck);
      } else {
        ++c.unknown;  // the stage origin is not known: no clock is written, never a guessed one
        ++clock_unknown_;
      }
    }
    if (!any) ++c.stale;  // the hook closed no new frame since the last record
    const Stats s = tracker.GetStats();
    c.unknown += uint32_t(s.unreadable + s.node_bad + s.overflow + s.late + s.old_calls - last_refused_);
    last_refused_ = s.unreadable + s.node_bad + s.overflow + s.late + s.old_calls;
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[arena_worlds calls " + std::to_string(s.calls) + ", recorded " + std::to_string(s.recorded) + ", frames " + std::to_string(s.frames) + ", foreign_palette " +
           std::to_string(s.foreign_palette) + ", node_assumed " + std::to_string(s.node_assumed) + ", node_bad " + std::to_string(s.node_bad) + ", unreadable " + std::to_string(s.unreadable) + ", overflow " + std::to_string(s.overflow) + ", late " + std::to_string(s.late) +
           ", old_calls " + std::to_string(s.old_calls) + ", clock_unknown " + std::to_string(clock_unknown_) + "]";
  }
  Tracker tracker;

 private:
  FrameBuffer scratch_;
  uint64_t last_refused_ = 0, clock_unknown_ = 0;
};

inline WorldContributor& Worlds() {
  static WorldContributor w;
  return w;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterArenaWorlds() {  // idempotent; the hooks do nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&Worlds());
}
inline bool ArenaWorldsRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::arenaworld
