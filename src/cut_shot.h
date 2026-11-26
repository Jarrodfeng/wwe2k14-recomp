// S111 SHOTDIET: event-triggered screenshots (input script `screenshot_on_cut N PREFIX`). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game / SDK includes (the unit drives it).
//
// WHAT: while on, the run writes a picture for the frame BEFORE each camera cut, the cut's own frame, and the frame N distinct frames after it (default 3), and nothing else.
// THE CUT SIGNAL is the host's own camera test on the scene record (scene_host.cpp: CameraJumped(previous drawn tick's camera, this tick's): the record's cut flag, an eye jump over 200 units, a turn over
// 15 degrees, a zoom over 10 %; the SAME test the tween uses to refuse blending across a cut), not an image difference. It is per drawn tick of the host, so it sees every cut the game hands the renderer.
// THE PICTURES come from the host thread: with the watch on, SceneHost reads back the presented picture of each drawn tick (one copy in flight; a tick that finds one still in flight is not read, counted
// by the host's `cut_watch_skipped`, and its cut is carried to the next tick read: CutLatch below) and calls the sink with (guest frame, cut flag, pixels). This machine decides which of them are written.
// The "frame before" is the previous DISTINCT picture READ, kept in memory (one frame, a shared pointer; nothing else is retained).
// A REPEATED guest frame (the host presents the same guest frame twice: a repeated presented frame) is skipped, so every written picture is a distinct guest frame and a frame is written at most once
// (a +N frame that is the next cut's "before" is one file).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "shot_policy.h"   // S111 SHOTDIET review N2: the writes-in-flight gate the host's watch also reads

namespace wwe2k14::cut_shot {

// One presented picture as the host read it back (RGBA8 rows, tight; the display curve is applied by the writer).
struct Picture {
  uint64_t frame = 0;   // the guest frame this picture is (the host's ShotAnswer::frame)
  bool cut = false;     // the camera cut test said this tick starts a new shot
  uint32_t w = 0, h = 0;
  std::shared_ptr<const std::vector<uint8_t>> rgba;
  std::array<uint8_t, 1024> curve{};
  bool curve_set = false;
};

// The watch: the host thread loads g_watch once per drawn tick (one relaxed load when off); the sink is called on the host thread and must only hand work over.
inline std::atomic<bool> g_watch{false};
using Sink = std::function<void(const Picture&)>;
inline std::mutex g_mu;
inline std::shared_ptr<Sink> g_sink;
inline void SetSink(std::shared_ptr<Sink> sink) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_sink = std::move(sink);
}
inline void Deliver(const Picture& p) {
  std::shared_ptr<Sink> s;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    s = g_sink;
  }
  if (s && *s) (*s)(p);
}

// Test plants (the unit only): 1 = repeated guest frames are not skipped; 2 = no "before" frame; 3 = the +N frame is counted in guest frames, not in distinct frames.
inline int g_plant = 0;

// S111 SHOTDIET review: the host's cut latch. The host reads back at most one tick's picture at a time, so a drawn tick can go unread (its copy would overlap the one in flight; the copy of tick T
// usually lands at T+1's start, but not always). The cut test of an unread tick is KEPT and handed to the next tick that is read, so a cut is never lost with its picture: the "at" picture is then the
// first picture read after the cut. Plant 4 (the unit only): an unread tick's cut is dropped (the host before this review).
struct CutLatch {
  bool pending = false;
  void Unread(bool cut) {   // this tick's picture is not read back
    if (g_plant != 4) pending = pending || cut;
  }
  bool Read(bool cut) {     // this tick's picture is read back: the cut flag it carries
    const bool c = cut || pending;
    pending = false;
    return c;
  }
};

struct Out {
  enum Kind { kBefore, kAt, kAfter } kind = kAt;
  uint64_t cut_no = 0;    // 1-based, in the order the cuts were seen
  uint64_t frame = 0;     // the guest frame whose picture is written (kBefore: the kept previous picture)
  uint32_t offset = 0;    // kAfter: frames after the cut
};

struct Fed {
  bool repeat = false;    // the same guest frame as the last one: skipped, nothing else happens
  bool cut = false;       // a cut was accepted at this frame
  uint64_t cut_no = 0;
  uint64_t prev_frame = 0;
  bool have_prev = false;
};

class Machine {
 public:
  explicit Machine(uint32_t after = 3) : after_(after) {}
  // One presented picture. Appends what to write to `out`: kBefore = the previous kept picture, kAt / kAfter = THIS picture.
  Fed Feed(uint64_t frame, bool cut, std::vector<Out>& out) {
    Fed r;
    if (have_cur_ && frame == cur_frame_ && g_plant != 1) {
      r.repeat = true;
      return r;
    }
    r.have_prev = have_cur_, r.prev_frame = cur_frame_;
    const uint64_t prev_d = d_;
    have_cur_ = true, cur_frame_ = frame, ++d_;
    bool emitted = false;
    if (cut && r.have_prev) {
      r.cut = true, r.cut_no = ++cuts_;
      if (g_plant != 2 && prev_d > last_emit_d_) out.push_back({Out::kBefore, r.cut_no, r.prev_frame, 0});
      out.push_back({Out::kAt, r.cut_no, frame, 0});
      emitted = true;
      if (after_) want_[g_plant == 3 ? frame + after_ : d_ + after_] = r.cut_no;
    }
    const uint64_t key = g_plant == 3 ? frame : d_;
    auto it = want_.find(key);
    if (it != want_.end()) {
      if (!emitted) out.push_back({Out::kAfter, it->second, frame, after_}), emitted = true;
      want_.erase(it);
    }
    if (emitted) last_emit_d_ = d_;
    return r;
  }
  uint64_t Cuts() const { return cuts_; }

 private:
  uint32_t after_;
  bool have_cur_ = false;
  uint64_t cur_frame_ = 0, d_ = 0, last_emit_d_ = 0, cuts_ = 0;   // d_ = the number of distinct frames seen (the current one's index, from 1)
  std::unordered_map<uint64_t, uint64_t> want_;
};

}  // namespace wwe2k14::cut_shot
