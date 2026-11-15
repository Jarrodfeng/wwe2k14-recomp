// S102 U1 (docs\renderer\SWAP_TAIL.md): the latch of the frame-keyed trackers (lights, arena worlds, ring rows, casters, spheres).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game or SDK includes.
//
// The trackers close a frame's buffer themselves when a call of a NEWER frame arrives (RotateLocked). Their Emit takes "every closed frame not yet
// emitted" after CloseBefore(present frame). Once the update thread is released it records frame G+1 at once, its first call closes frame G, and a late
// Emit would hand frame G to record G (today it goes to record G+1). So the latch does Emit's own take while the update thread is parked:
// CloseBefore(G) and every TakeNext into the contributor's own list (O(1) swaps: the buffers' capacity cycles through the ring, as with Emit's scratch).
// Emit then reads that list. A buffer the latch took and no Emit consumed (a refused frame) goes out with the next Emit (Begin), as it would have stayed in the ring.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "../renderer_scene/live_scene.h"

namespace rdr_scene::live {

// kCap = the tracker's own ring (kRing): an unconsumed latch keeps at most that many frames, oldest dropped first and counted (review F2: a contributor
// that throws in every Emit would otherwise grow the list one buffer a frame for good).
template <class Buffer, size_t kCap>
class TrackerLatch {
 public:
  // The latch (the game threads parked): Emit's take, now.
  template <class Tracker>
  bool Take(Tracker& tracker, const FrameInfo& f) {
    tracker.CloseBefore(f.guest_frame);
    if (!full_) n_ = 0;   // an unconsumed latch (a refused record) keeps its buffers in front (oldest first)
    for (;;) {
      if (bufs_.size() <= n_) bufs_.emplace_back();   // n_ <= kCap: at most kCap + 1 buffers (the last one a spare)
      if (!tracker.TakeNext(bufs_[n_])) break;
      bool room = n_ < kCap;
#ifdef RDR_TAIL_PLANTS
      if (g_tail_plant == 100) room = true;   // planted (tail_order_test plant_throw_nocap): the uncapped latch of c59a315f
#endif
      if (room) {
        ++n_;
      } else {   // full: the oldest kept frame is dropped (it becomes the spare), as the tracker's own ring drops a frame Emit did not take; counted
        std::rotate(bufs_.begin(), bufs_.begin() + 1, bufs_.begin() + std::ptrdiff_t(kCap) + 1);
        ++dropped_;
      }
    }
    frame_ = f.guest_frame;
    full_ = true;
    return true;
  }
  // Emit: use the latch made for this frame, or take from the tracker as before (a plain EndFrame). A deferred emit that reaches the tracker is a violation.
  // A latch no Emit consumed (its frame's record was refused before this contributor's turn) is not lost: its buffers come first (oldest first), then
  // the tracker's, as a plain EndFrame would have found them all still in the ring.
  template <class Tracker>
  void Begin(Tracker& tracker, const FrameInfo& f, const char* name) {
    using_ = full_;
    const bool same = full_ && frame_ == f.guest_frame;
    full_ = false;
    next_ = 0;
    tracker_after_ = !same;
    if (same) return;
    if (InDeferredEmit()) NoteViolation(name);
    tracker.CloseBefore(f.guest_frame);
  }
  template <class Tracker>
  bool Next(Tracker& tracker, Buffer& out) {
    if (using_ && next_ < n_) {
      std::swap(out, bufs_[next_++]);
      return true;
    }
    return tracker_after_ && tracker.TakeNext(out);
  }

  uint64_t Dropped() const { return dropped_; }      // frames dropped from an unconsumed latch (the test reads it)
  size_t Kept() const { return bufs_.size(); }        // buffers allocated (at most kCap + 1)
  size_t Held() const { return n_; }                  // frames held for the next Emit (at most kCap)

 private:
  std::vector<Buffer> bufs_;
  uint64_t dropped_ = 0;
  size_t n_ = 0, next_ = 0;
  uint64_t frame_ = ~0ull;
  bool full_ = false, using_ = false, tracker_after_ = true;
};

// A contributor's one-shot latch flag for frame f (circles, plane, arena, wrestlers, crowd keep their own data next to it).
struct LatchMark {
  uint64_t frame = ~0ull;
  bool full = false;
  void Set(uint64_t f) { frame = f, full = true; }
  // Emit: true = the latch is this frame's (and is consumed). False = take from the shared state as before; a deferred emit doing so is a violation.
  bool Use(uint64_t f, const char* name) {
    const bool ok = full && frame == f;
    full = false;
    if (!ok && InDeferredEmit()) NoteViolation(name);
    return ok;
  }
};

}  // namespace rdr_scene::live
