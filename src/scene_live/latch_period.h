// The value a scene record takes from the latch WHOSE PALETTES IT TAKES (S111 CUTSTUTTER, docs\research\s111\CUTSTUTTER.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only and PURE (no game, SDK or renderer includes): inputs_contrib.h (the scene camera, the model-class rows), the node hooks scene_skip_nodes.h / arena_hooks.h (NodeClock) and tools\scene\inputs_unit.cpp include it.
//
// WHY: latch r runs after every update task of period r, so a value the update writes is period r's at latch r. Record r carries period r - 1: its palettes are the latch reads tagged hook + kPaletteLead
// taken with PickLimit (wrestler_contrib.h (rdr_scene::live::WrestlerContributor::Kept); S103 W1 measured latch r - 1's palette = the one stream r's draws read, bit for bit), its visibility, colours and piece links likewise (S111 VISLATCH /
// PERIODALL). The scene camera is such a value: the hand-off sub_823C79A8 (the only writer of the block [[0x83929A68] + 256] and of the thread-slot camera the body draw reads, sub_82BA86B8 +50 / +74) runs
// inside the composer sub_823C8548, update task sub_823C8B20 in group 5, BEFORE the pose tasks (groups 11..23) whose palettes the walk of the same period draws. So the walk that drew record r's palettes
// drew with the block handed in period r - 1, and the camera read at latch r is one period ahead. At a camera cut record r showed the NEW shot over the OLD shot's bodies, culled pieces, crowd classes
// and lights: one frame per cut, in cutscenes, entrances and director cuts alike (one composer, one hand-off).
// The rule: record r takes latch r - 1's value. Boundary (not a special case): when no latch of r - 1 was kept (the first latch of a run, a skipped latch) the record's own read is the only one there is.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace scene_inputs::period {

inline constexpr uint64_t kLead = 1;   // the same one-period lead as wrestler_contrib.h kPaletteLead
// Test plant (the unit only): 1 = the record takes its own latch's read (the old rule: the camera one period ahead of the palettes).
inline int g_plant = 0;

template <class T>
class OfPalette {
 public:
  // One latch of record `frame` (the latch thread only). `now` = this latch's read (by value: a caller that no longer needs it moves it in, S111 CUTREACH review: the model-class rows run to tens of KB on the parked
  // latch). Returns the value record `frame` carries. In steady state nothing is copied (the kept value moves out, `now` moves in); at the boundary the record's own read is the one copy.
  T Pick(uint64_t frame, T now) {
    latches_.fetch_add(1, std::memory_order_relaxed);
    T out;
    if (g_plant != 1 && have_ && frame >= kLead && prev_frame_ == frame - kLead) {
      out = std::move(prev_);
      previous_.fetch_add(1, std::memory_order_relaxed);
    } else {
      out = now;
      (have_ ? own_gap_ : own_first_).fetch_add(1, std::memory_order_relaxed);
    }
    prev_ = std::move(now), prev_frame_ = frame, have_ = true;
    return out;
  }
  // The census (read on another thread): `previous` = records that took the latch before theirs; own_first / own_gap = the boundary (no latch of r - 1 kept).
  std::string CensusLine(const char* name) const {
    const auto n = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
    return std::string(name) + ": latches " + n(latches_) + " previous " + n(previous_) + " own_first " + n(own_first_) + " own_gap " + n(own_gap_);
  }

 private:
  T prev_{};
  uint64_t prev_frame_ = 0;
  bool have_ = false;
  std::atomic<uint64_t> latches_{0}, previous_{0}, own_first_{0}, own_gap_{0};
};

// S111 CUTREACH review (docs\research\s111\CUTREACH.md, Review): which clock a draw node runs on (FRAME_CONTRACT 3, the draw-half clock), measured the way particle_source.h measures the particle node. The node's
// hook notes the hook clock at its call; every latch buckets the last note against its own hook (HookFrameOfRecord of the record frame): 0 = the clock of the model draws (model-class: record r takes latch r - 1),
// +1 = the particle node's draw-half clock (walk-time: record r takes its own latch), before = no call since the last latch (a skipped node / walk), other, never = no call yet. Log only: the census reads it.
class NodeClock {
 public:
  struct Counts {
    uint64_t b0 = 0, b1 = 0, before = 0, other = 0, never = 0;
  };
  void Note(uint64_t clock) { noted_.store(clock + 1, std::memory_order_relaxed); }   // the node's hook (a game thread): one relaxed store (0 = never)
  void AtLatch(uint64_t hook) {   // the latch thread
    const uint64_t n = noted_.load(std::memory_order_relaxed);
    if (!n) {
      never_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint64_t noted = n - 1;
    (noted == hook ? b0_ : noted == hook + 1 ? b1_ : noted < hook ? before_ : other_).fetch_add(1, std::memory_order_relaxed);
  }
  Counts Read() const {
    Counts c;
    c.b0 = b0_.load(std::memory_order_relaxed), c.b1 = b1_.load(std::memory_order_relaxed), c.before = before_.load(std::memory_order_relaxed), c.other = other_.load(std::memory_order_relaxed),
    c.never = never_.load(std::memory_order_relaxed);
    return c;
  }
  // "NAME: 0 N +1 N before N other N never N" (not the particles' "noted-hook" wording: a log-wide search for that line must not land here)
  std::string CensusLine(const char* name) const {
    const Counts c = Read();
    const auto s = [](uint64_t v) { return std::to_string((unsigned long long)v); };
    return std::string(name) + ": 0 " + s(c.b0) + " +1 " + s(c.b1) + " before " + s(c.before) + " other " + s(c.other) + " never " + s(c.never);
  }

 private:
  std::atomic<uint64_t> noted_{0}, b0_{0}, b1_{0}, before_{0}, other_{0}, never_{0};
};
inline NodeClock g_post_node_clock;   // the post module's draw node sub_82C1E840 (scene_skip_nodes.h): the post settings (kScPostParams) stay on latch r until it says +1 or 0
inline NodeClock g_page_node_clock;   // the stage page pass sub_825BB0E0 (arena_hooks.h; the same walker site 0x82C2FB10 as the particle node): the entrance pools, the follow spot, the track sprites

}  // namespace scene_inputs::period
