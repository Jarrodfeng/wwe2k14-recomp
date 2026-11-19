// The plane shadow chain as a live contributor to the scene record (Session 101, lane 139; docs/renderer/SHADOW_PLANE.md; sections kScPlaneFrames / kScPlaneDraws).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes. A Contributor named "plane": Emit appends every frame block and draw the hook gathered since the last
// record (each stamped with its own guest frame). Registered once at startup, only with --wwe2k14_scene_records plus a plane switch (wwe2k14_app.h).
#pragma once
#include <atomic>
#include <algorithm>
#include "frame_delta.h"   // S103 A3 (F1)
#include <cstring>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "plane_hook.h"
#include "tail_latch.h"  // S102 U1

namespace rdr_scene::live::plane {

// The writer checks its own tables (live::Validate does not look at them): a table that does not check out is dropped, only the plane tables, and counted. Returns true when dropped.
inline bool DropIfBad(SceneTables& t, Tracker* tracker = nullptr) {
  if (PlaneTablesProblem(t).empty()) return false;
  t.plane_frames.clear();
  t.plane_draws.clear();
  if (tracker) tracker->CountDropped();
  return true;
}

class PlaneContributor : public Contributor {
 public:
  const char* Name() const override { return "plane"; }
  bool Latch(const FrameInfo& fi) override {   // S102 U1 (tail_latch.h): Emit's take, while the game threads are parked
    tracker.Take(frames_, draws_);
    latch_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    if (!latch_.Use(fi.guest_frame, Name())) tracker.Take(frames_, draws_);   // S102 U1: or what the latch took (frames_ / draws_ hold it)
    if (draws_.empty() && frames_.empty()) {
      ++empty_emits_;   // the hook gathered nothing since the last record: this record carries no plane tables (the contributor's own count, apart from the tracker's)
      return;
    }
    // S103 A2 (S7): the inputs contributor (registered first) already put the LATCH's PlaneFrame of a frame into t.plane_frames. For each frame the walk's recorder wrote with the same stamp: compare the two
    // bit for bit (fade / fit / camera: counted equal / differs, the proof that the latch block is the walk's block, a mirror-run read), then keep the WALK's block and drop the latch's. Other frames append.
    for (const PlaneFrame& w : frames_) {
      bool merged = false;
      for (PlaneFrame& l : t.plane_frames) {
        if (l.frame != w.frame) continue;
        const bool same = std::memcmp(l.fade, w.fade, sizeof l.fade) == 0 && std::memcmp(l.fit_matrix, w.fit_matrix, sizeof l.fit_matrix) == 0 && std::memcmp(l.camera, w.camera, sizeof l.camera) == 0;
        (same ? latch_same_ : latch_differs_) += 1;
        const uint32_t nd = l.flags & kPlaneNoDraw;   // S103 PB: the latch saw the game's no-draw frame: the walk's block holds the same raw inf, keep the flag
        l = w;
        if (nd) l.flags |= nd, l.have &= ~uint32_t(kPlaneHaveFit);
        merged = true;
        break;
      }
      if (!merged) {
        bool one_off = false;   // F1: a latch frame one off = the draw-half clock mismatch the census must show
        for (const PlaneFrame& l : t.plane_frames)
          if (l.frame + 1 == w.frame || w.frame + 1 == l.frame) { delta_.Note(int64_t(w.frame) - int64_t(l.frame)); one_off = true; break; }
        if (!one_off) delta_.Never();
        t.plane_frames.push_back(w);
      } else {
        delta_.Note(0);
      }
    }
    // S103 A3 (S8): the inputs contributor (registered first) put latch-derived mask rows (kPlaneFromLatch) of the proxy list copy into t.plane_draws. Where the walk recorded draws of the same hook frame, those
    // rows are the walk's to give: drop the latch rows of that frame (a mirror frame must not draw a proxy twice); otherwise they stay (an OURS frame has no walk rows).
    if (!draws_.empty()) {
      t.plane_draws.erase(std::remove_if(t.plane_draws.begin(), t.plane_draws.end(), [&](const PlaneDraw& d) {
        if (!(d.flags & kPlaneFromLatch)) return false;
        for (const PlaneDraw& w : draws_) if (w.frame == d.frame) { ++latch_rows_dropped_; return true; }
        return false;
      }), t.plane_draws.end());
    }
    t.plane_draws.insert(t.plane_draws.end(), draws_.begin(), draws_.end());
    c.objects += uint32_t(draws_.size());
    DropIfBad(t, &tracker);
    frames_.clear();
    draws_.clear();
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[plane calls " + std::to_string(s.calls) + ", mask rows " + std::to_string(s.rows_mask) + ", blur rows " + std::to_string(s.rows_blur) + ", frames " + std::to_string(s.frames) +
           ", mask draws " + std::to_string(s.mask_draws) + ", blur draws " + std::to_string(s.blur_draws) + ", incomplete frames " + std::to_string(s.incomplete_frames) + ", orphans " + std::to_string(s.orphans) +
           ", unreadable " + std::to_string(s.unreadable) + ", overflow " + std::to_string(s.overflow) + ", tables dropped " + std::to_string(s.dropped_tables) + ", " + delta_.Str() + ", latch_proxy_rows_dropped " + std::to_string(latch_rows_dropped_) + ", latch_equal " + std::to_string(latch_same_) + ", latch_differs " + std::to_string(latch_differs_) + ", empty emits " + std::to_string(empty_emits_) + "]";
  }
  Tracker tracker;
  uint64_t LatchSame() const { return latch_same_; }
  uint64_t LatchDiffers() const { return latch_differs_; }
  uint64_t LatchRowsDropped() const { return latch_rows_dropped_; }
  void TestInjectDraw(const PlaneDraw& d) { draws_.push_back(d); }   // tests only: a draw the walk recorded
  void TestInject(const PlaneFrame& f) { frames_.push_back(f); }   // tests only: a frame the walk recorder wrote (after Latch, before Emit)

 private:
  LatchMark latch_;   // S102 U1
  std::vector<PlaneFrame> frames_;
  std::vector<PlaneDraw> draws_;
  uint64_t empty_emits_ = 0;
  uint64_t latch_same_ = 0, latch_differs_ = 0, latch_rows_dropped_ = 0;
  DeltaCensus delta_;   // walk frame - latch frame (draw-half clock; 0 expected)
};

inline PlaneContributor& The() {
  static PlaneContributor c;
  return c;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterPlane() {   // idempotent; the hooks do nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&The());
}
inline bool PlaneRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::plane
