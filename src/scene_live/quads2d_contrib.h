// Quads2DContrib (Session 103, lane L2a, landing 2 piece P1; docs\renderer\LANDING2_PLAN.md section 1): the 2D layer's rows into the frame's scene record. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The taps (quads2d_tap.h, behind --wwe2k14_rdr_2d_record) leave rows and holes in the store; this contributor moves record j's share into the record AT THE LATCH (the update thread parked):
// q2d::TakeFrame(store, j): the rows of frames <= j (Drain(j): record frame j itself, never scene_frame::HookFrameOf(j), never j - 1: R16's reasoning [L]), the holes of the same frames, the pair
// census by (row.frame - j). Only rows stamped j are kept; older rows and bad rows are dropped and counted as holes (so that frame keeps the stream's 2D). t.quads2d = the rows (call order),
// t.quad2d_holes = the hole count (0 = every 2D draw seen this frame is a row). Emit reads only what Latch took (no guest memory, no shared state): it may run deferred.
// Registered once at start (wwe2k14_app.h, inside the scene-records block) only when --wwe2k14_rdr_2d_record is on.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/record_io.h"
#include "../renderer_scene/rows_quad2d.h"   // S106 G4-2
#include "quads2d_tap.h"

namespace wwe2k14::q2d_tap {

class Quads2DContributor : public rdr_scene::live::Contributor {
 public:
  const char* Name() const override { return "quads2d"; }
  bool Latch(const rdr_scene::live::FrameInfo& f) override {
    Take(f.guest_frame);
    latched_ = true;
    return true;
  }
  void Emit(const rdr_scene::live::FrameInfo& f, rdr_scene::SceneTables& t, rdr_scene::live::Counters& c) override {
    if (!latched_) Take(f.guest_frame);   // no latch ran (the tail decision off): the boundary itself, the same moment
    latched_ = false;
    if (take_frame_ != f.guest_frame) { ++c.stale, stale_.fetch_add(1, std::memory_order_relaxed); return; }   // (never expected: Latch and Emit carry the same frame)
    t.quads2d.assign(take_.rows.begin(), take_.rows.end());
    t.quad2d_holes = take_.hole_count;
    c.objects += uint32_t(take_.rows.size());
  }
  void Census(std::string& out) override {
    const auto n = [](const std::atomic<uint64_t>& v) { return (unsigned long long)v.load(std::memory_order_relaxed); };
    char line[400];
    std::snprintf(line, sizeof line, ", quads2d: frames %llu (whole %llu, with holes %llu), rows %llu, holes %llu, pair (0 %llu, -1 %llu, +1 %llu, other %llu), stale %llu", n(frames_), n(whole_),
                  n(with_holes_), n(rows_), n(holes_), n(same_), n(prev_), n(next_), n(other_), n(stale_));
    out += line;
    out += " [" + StateReport().substr(2) + "]";
    {   // S105 A8: the glyph refusals the old line never named (q2d_tap::Report() had no callers): a glyph row whose register file lacked c0 / c1 / c4 / c28 (glyph_no_shadow) or took one stamped with another guest frame (glyph_stale),
      // and a T_RectColor draw whose corner colour was not a whole byte (bad_corner)
      const auto& gc = g_counters;
      char g[240];
      std::snprintf(g, sizeof g, " [glyph: rows %llu, no_shadow %llu, stale %llu; rect_color bad_corner %llu]", n(gc.glyph_rows), n(gc.glyph_no_shadow), n(gc.glyph_stale), n(gc.bad_corner));
      out += g;
    }
  }

 private:
  void Take(uint64_t j) {
    rdr_scene::q2d::TakeFrame(TheStore(), j, [](const rdr_scene::Quad2D& q) { return rdr_scene::Quad2DRowProblem(q).empty(); }, take_);
    take_frame_ = j;
    frames_.fetch_add(1, std::memory_order_relaxed);
    (take_.hole_count ? with_holes_ : whole_).fetch_add(1, std::memory_order_relaxed);
    rows_.fetch_add(take_.rows.size(), std::memory_order_relaxed), holes_.fetch_add(take_.hole_count, std::memory_order_relaxed);
    same_.fetch_add(take_.pairs.same, std::memory_order_relaxed), prev_.fetch_add(take_.pairs.prev, std::memory_order_relaxed);
    next_.fetch_add(take_.pairs.next, std::memory_order_relaxed), other_.fetch_add(take_.pairs.other, std::memory_order_relaxed);
    if (fallback_census::Enabled()) fallback_census::NotePairs(take_.pairs.same, take_.pairs.prev, take_.pairs.next, take_.pairs.other);   // S103 L2aF (review M3): the census line's pair field (rows by row.frame - j)
  }
  rdr_scene::q2d::FrameTake take_;
  uint64_t take_frame_ = ~0ull;
  bool latched_ = false;
  std::atomic<uint64_t> frames_{0}, whole_{0}, with_holes_{0}, rows_{0}, holes_{0}, same_{0}, prev_{0}, next_{0}, other_{0}, stale_{0};
};

inline Quads2DContributor& Contributor() {
  static Quads2DContributor c;
  return c;
}
// Once, at start, before any sink attaches (live_scene.h). Only with --wwe2k14_rdr_2d_record on: off registers nothing.
inline void RegisterContributor() {
  static bool done = false;
  if (done || !Active()) return;
  done = true;
  rdr_scene::live::Register(&Contributor());
}

}  // namespace wwe2k14::q2d_tap
