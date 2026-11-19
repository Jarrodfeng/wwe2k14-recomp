// The circle blob shadows as a live contributor to the scene record (Session 101, lane 136; docs/renderer/SHADOW_CIRCLES.md; sections kScCircleShadows / kScCircleMesh).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes. A Contributor named "circles": Emit appends every entry the hook gathered since the
// last record (each stamped with its own guest frame) and, with them, the disc mesh. Registered once at startup, only with --wwe2k14_scene_records plus a circles switch (wwe2k14_app.h).
#pragma once
#include <atomic>
#include <cstring>
#include <string>

#include "../renderer_scene/live_scene.h"
#include "circle_hook.h"
#include "frame_delta.h"   // S103 A3 (F1)
#include "../renderer_game/node_switch.h"   // S103 A3 (F4): the stored decision
#include "tail_latch.h"  // S102 U1

namespace rdr_scene::live::circles {

class CircleContributor : public Contributor {
 public:
  const char* Name() const override { return "circles"; }
  bool Latch(const FrameInfo& fi) override {   // S102 U1 (tail_latch.h): Emit's take, while the game threads are parked
    tracker.Take(scratch_, &latch_mesh_, latch_mesh_present_, /*always_mesh=*/true);
    latch_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    bool mesh_present = false;
    CircleMesh mesh;
    if (latch_.Use(fi.guest_frame, Name())) mesh = latch_mesh_, mesh_present = latch_mesh_present_;   // S102 U1: what the latch took (scratch_ holds its entries)
    else tracker.Take(scratch_, &mesh, mesh_present, /*always_mesh=*/true);
    if (scratch_.empty()) {
      // S103 A3 (F4): the walk recorded nothing for this record. If the stored decision for the record's hook frame is NOT Ours the game WALKED it, and any latch row in the record is a blob the walk did not draw
      // (a false blob): counted as latch_only_frames (the census a mirror run reads as 0). Under OURS the same state is the normal one and counts nothing.
      if (!t.circle_shadows.empty() && fi.guest_frame >= 1) {
        bool walked = true;   // mode 1 has no Scene3D stamp at all: a frame is WALKED unless a stamp says Ours (review must 3)
        for (const rdr_own::OwnedPassStamp& st : rdr_own::g_board.Take(fi.guest_frame - 1))
          if (st.pass == uint8_t(rdr_own::Pass::Scene3D) && st.decision == uint8_t(rdr_own::Decision::Ours)) walked = false;
        if (walked)
          for (const CircleShadow& l : t.circle_shadows)
            if (l.flags & kCircleFromLatch) { ++latch_only_frames_; break; }
      }
      // S103 A3 (S6): under OURS the walk's tracker gathers nothing; the latch's rows (inputs contributor, emitted before us) still need the disc mesh the tracker kept from the last walked call.
      if (!t.circle_shadows.empty() && mesh_present && t.circle_mesh.empty()) {
        t.circle_mesh.push_back(mesh);
        return;
      }
      ++c.stale;   // the hook gathered no entry since the last record: this record carries no circle tables
      return;
    }
    // S103 A3 (S6): the inputs contributor (registered first) already put the LATCH's rows for this record's hook frame into t.circle_shadows, flagged kCircleFromLatch. For each walk row: find the LATCH row
    // of the same (frame, slot) (only latch rows match: a duplicate walk row never pairs with another walk row), compare c0 / c4-c7 / the points the latch could read, count equal / differs (the proof that the
    // latch rows are the walk's rows, a mirror-run read) and keep the WALK's row. A walk row with no latch row is `walk_only`; a latch row of a frame the walk recorded but of a slot it did not is `latch_only`
    // (a blob the game does not draw): both are what a mirror run must read as 0.
    std::vector<uint32_t> walk_frames;
    for (const CircleShadow& w : scratch_) {
      bool merged = false;
      for (CircleShadow& l : t.circle_shadows) {
        if (!(l.flags & kCircleFromLatch) || l.frame != w.frame || l.slot != w.slot) continue;
        bool same = std::memcmp(l.shadow_param, w.shadow_param, sizeof l.shadow_param) == 0 && std::memcmp(l.view_proj, w.view_proj, sizeof l.view_proj) == 0 && l.character == w.character;
        for (uint32_t k = 0; same && k < kCirclePoints; ++k) {
          const bool latch_has = l.points[k][3] != 0.f || l.points[k][0] != 0.f || l.points[k][1] != 0.f || l.points[k][2] != 0.f;   // an unset point was left at zero
          if (latch_has && std::memcmp(l.points[k], w.points[k], sizeof l.points[k]) != 0) same = false;
        }
        (same ? latch_same_ : latch_differs_) += 1;
        l = w;
        merged = true;
        break;
      }
      if (!merged) {
        ++walk_only_;
        bool one_off = false;   // F1: a latch row of the same slot one frame off = the draw-half clock mismatch
        for (const CircleShadow& l : t.circle_shadows)
          if ((l.flags & kCircleFromLatch) && l.slot == w.slot && (l.frame + 1 == w.frame || w.frame + 1 == l.frame)) { delta_.Note(int64_t(w.frame) - int64_t(l.frame)); one_off = true; break; }
        if (!one_off) delta_.Never();
      } else {
        delta_.Note(0);
      }
      if (!merged) t.circle_shadows.push_back(w);
      walk_frames.push_back(w.frame);
    }
    for (const CircleShadow& l : t.circle_shadows) {
      if (!(l.flags & kCircleFromLatch)) continue;
      for (uint32_t wf : walk_frames)
        if (wf == l.frame) { ++latch_only_; break; }
    }
    c.objects += uint32_t(scratch_.size());
    if (mesh_present) t.circle_mesh.push_back(mesh);
    else ++c.unknown;   // entries without a mesh: the mesh could not be read from the object (counted; the renderer keeps the stream's draws)
    scratch_.clear();
  }
  void Census(std::string& out) override {
    const Stats s = tracker.GetStats();
    out += "[circles calls " + std::to_string(s.calls) + ", viewproj " + std::to_string(s.viewproj) + ", params " + std::to_string(s.params) + ", entries " + std::to_string(s.entries) +
           ", orphans " + std::to_string(s.orphans) + ", unreadable " + std::to_string(s.unreadable) + ", bad_slot " + std::to_string(s.bad_slot) + ", overflow " + std::to_string(s.overflow) +
           ", latch_equal " + std::to_string(latch_same_) + ", latch_differs " + std::to_string(latch_differs_) + ", latch_only_frames " + std::to_string(latch_only_frames_) + ", " + delta_.Str() + ", latch_only " + std::to_string(latch_only_) + ", walk_only " + std::to_string(walk_only_) + ", mesh reads " + std::to_string(s.mesh_reads) + " ok " + std::to_string(s.mesh_ok) + " bad " + std::to_string(s.mesh_bad) + "]";
  }
  Tracker tracker;
  uint64_t LatchSame() const { return latch_same_; }
  uint64_t LatchDiffers() const { return latch_differs_; }
  uint64_t LatchOnly() const { return latch_only_; }
  uint64_t WalkOnly() const { return walk_only_; }
  uint64_t LatchOnlyFrames() const { return latch_only_frames_; }
  void TestInject(const CircleShadow& e) { scratch_.push_back(e); }   // tests only: an entry the walk's tracker recorded (after Latch, before Emit)

 private:
  std::vector<CircleShadow> scratch_;
  LatchMark latch_;   // S102 U1
  CircleMesh latch_mesh_;
  bool latch_mesh_present_ = false;
  uint64_t latch_same_ = 0, latch_differs_ = 0, latch_only_ = 0, walk_only_ = 0, latch_only_frames_ = 0;
  DeltaCensus delta_;   // walk row frame - latch row frame (draw-half clock; 0 expected)   // S103 A3: the latch's rows against the walk's (Emit)
};

inline CircleContributor& The() {
  static CircleContributor c;
  return c;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterCircles() {   // idempotent; the op-21 hook does nothing until this has run
  if (!RegisteredFlag().exchange(true)) Register(&The());
}
inline bool CirclesRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::circles
