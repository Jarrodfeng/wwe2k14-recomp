// The arena's evaluated worlds, the game-side half (Session 101, lane 130). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only core: no game or SDK includes. The game plumbing is arena_world_glue.h; tools\scene\arena_world_test.cpp drives this same code with a fake guest.
//
// At every model call (the existing mid-asm hook wwe2k14_model_entry, sub_82C04DF0 at 0x82C04E1C: r3 = model object, r5 = the palette the packer reads) it keeps one
// kAwObject entry per (frame, object, r5): the world rows of the matrix the packer cuts c76..c78 from, stamped with the DRAWING thread's guest frame.
//  * r5 is ALWAYS the array read: the packer stores it at 0x82C04E14 (stw r5,29044(r1)) and reloads it at 0x82C0A01C; sub_82C13EC8 (mr r5,r4; b 0x82C04DF0) passes the
//    caller's array. r5 == [model+200] is only a flag (kAwForeignPalette when not). [review 1]
//  * The matrix is node n of that palette: r5 + 64 (n - 1), n = the first entry of part 0's bone list: r21 = [model+824] + 184 * part, indices are words [r21+12+4k] (1-based),
//    the count at [r21+8] (listing 0x82C05940-0x82C05974; the [model+220] vtable route used when +824 is null is NOT followed: node 1 is assumed and kAwNodeAssumed is set
//    and counted `node_assumed`). `slot` of the entry = n. The palette holds skinning matrices (world x inverse bind), not the node world. [review 2]
// Parts of one object share a world in a frame [C, 98 of 98 sampled arena objects]; only part 0 is read. J1 compares every stream draw's c76..c78 with the entry.
// The stage clock origin is set by OnStageSetup (CStage::MotionSetup exit, arena_hooks.h) and SNAPSHOT into each frame's buffer when the frame closes, so a later set-up
// (a reload, a second stage) cannot change the clock of a frame already closed. [review 6]
// Frames, ring, TakeNext: as light_state_hook.h (a call in a later frame closes the buffer; a closed frame overwritten before Emit took it is counted `late`).
// Size: every model call of the frame is kept (~400 objects, 68 bytes each, ~27 KB a frame); the arena's own objects are not distinguishable at the hook, so the record
// carries them all and the join (DrawOrigin::object) picks. J1 measures the record size and the frame time. [review 9]
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "arena_world.h"

namespace rdr_scene::live::arenaworld {

inline constexpr uint32_t kMaxObjects = 1024;  // per frame
inline constexpr uint32_t kRing = 4;
inline constexpr uint32_t kMaxNode = 1024;     // a node index at or above this is refused (counted node_bad)

// Tests only: a planted fault must fail its NAMED check (the shipped code carries each as one branch).
enum class Plant : uint32_t { None = 0, PlacedIdentity, MovingStatic, ClockFrozen, RefuseForeignPalette, Node1Assumed, OriginAfterReload };

struct Stats {
  uint64_t calls = 0, recorded = 0, foreign_palette = 0, node_assumed = 0, node_bad = 0, unreadable = 0, overflow = 0, old_calls = 0, late = 0, frames = 0;
};

struct FrameBuffer {
  uint64_t frame = 0;
  uint32_t origin = 0;         // the stage clock origin when this frame closed
  bool origin_known = false;
  std::vector<ArenaWorld> worlds;
  bool valid = false;
};

class Tracker {
 public:
  Plant plant = Plant::None;

  // `read_word(addr, v)` / `read_mat(addr, float[16] host order)` are the guest reads (the fake in the test, wrestlers::ReadWord / ReadMatrices in the game).
  template <class ReadWordFn, class ReadMatFn>
  void OnModelCall(uint32_t model, uint32_t palette_ptr, uint64_t frame, ReadWordFn read_word, ReadMatFn read_mat) {
    uint32_t field = 0, desc_base = 0, bones = 0, node = 0;
    const bool pal = palette_ptr != 0;
    const bool f200 = pal && read_word(model + 200, field);
    const bool foreign = f200 && field != palette_ptr;  // not [model+200]: another caller's array; still what the packer reads
    uint32_t flags = foreign ? kAwForeignPalette : 0;
    bool node_bad = false;
    if (pal && !(read_word(model + 824, desc_base) && desc_base >= 0x10000 && read_word(desc_base + 8, bones) && bones > 0 && read_word(desc_base + 12, node))) {
      node = 1;  // no readable part descriptor: node 1 assumed
      flags |= kAwNodeAssumed;
    } else if (pal && (node == 0 || node >= kMaxNode)) {
      node_bad = true;
    }
    if (plant == Plant::Node1Assumed) node = 1;
    float m[16];
    const bool ok = pal && !node_bad && read_mat(palette_ptr + 64u * (node - 1u), m);
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.calls;
    if (!Advance(frame)) { ++stats_.old_calls; return; }
    if (plant == Plant::RefuseForeignPalette && foreign) { ++stats_.foreign_palette; return; }
    if (foreign) ++stats_.foreign_palette;
    if (flags & kAwNodeAssumed) ++stats_.node_assumed;
    if (node_bad) { ++stats_.node_bad; return; }
    if (!ok) { ++stats_.unreadable; return; }
    const uint64_t key = (uint64_t(model) << 32) | palette_ptr;  // dedupe on (object, r5): the parts of one object share it, a caller with another array does not
    if (index_.count(key)) return;
    if (cur_.worlds.size() >= kMaxObjects) { ++stats_.overflow; return; }
    ArenaWorld w;
    w.kind = kAwObject;
    w.object = model;
    w.frame = uint32_t(frame);
    w.slot = uint16_t(node);
    w.origin = flags;
    RowsFromMatrix(m, w.world);
    const bool pure_translation = w.world[0] == 1.f && w.world[5] == 1.f && w.world[10] == 1.f && w.world[1] == 0.f && w.world[2] == 0.f && w.world[4] == 0.f &&
                                  w.world[6] == 0.f && w.world[8] == 0.f && w.world[9] == 0.f;
    if (plant == Plant::PlacedIdentity && pure_translation && std::fabs(w.world[3]) >= 400.f && std::fabs(w.world[11]) >= 400.f) {  // the record's identity kept for the placed slots
      const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      std::memcpy(w.world, id, sizeof id);
    }
    if (plant == Plant::MovingStatic) {  // the frame-0 transform held for ever
      auto it = first_.find(model);
      if (it == first_.end()) first_.emplace(model, std::vector<float>(w.world, w.world + 12));
      else std::memcpy(w.world, it->second.data(), sizeof w.world);
    }
    index_.emplace(key, uint32_t(cur_.worlds.size()));
    cur_.worlds.push_back(w);
    ++stats_.recorded;
  }

  // CStage::MotionSetup exit: the frame the stage was set up in. Later calls overwrite (a new stage = a new origin) but never reach a frame already closed.
  void OnStageSetup(uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    origin_ = uint32_t(frame);
    origin_known_ = true;
    clock_first_ = false;
  }
  bool Origin(uint32_t& o) const {
    std::lock_guard<std::mutex> lk(mu_);
    o = origin_;
    return origin_known_;
  }

  // lane 152 (review B1): the record writer closes every open frame older than the present's guest frame, so a frame's stamps never wait for a LATER call to be closed (that held only by a race).
  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_.valid && cur_.frame < frame) Advance(frame);
  }
  // The oldest closed frame not yet taken; false when none. Swaps the buffer out (no copy).
  bool TakeNext(FrameBuffer& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (taken_ >= closed_count_) return false;
    FrameBuffer& b = ring_[taken_ % kRing];
    out = std::move(b);
    b = FrameBuffer();
    ++taken_;
    return true;
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
  }
  // The clock value of a closed frame and the origin it used (the buffer's snapshot; OriginAfterReload plants the live origin instead). ClockFrozen plants the first value.
  bool ClockFor(const FrameBuffer& b, float& out, uint32_t& origin_used) {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t o = b.origin;
    bool known = b.origin_known;
    if (plant == Plant::OriginAfterReload) o = origin_, known = origin_known_;
    if (!ClockAt(uint32_t(b.frame), o, known, out)) return false;
    origin_used = o;
    if (plant == Plant::ClockFrozen) {
      if (!clock_first_) clock_first_ = true, clock_frozen_ = out;
      out = clock_frozen_;
    }
    return true;
  }

 private:
  // Returns false for a call of an already closed frame.
  bool Advance(uint64_t frame) {
    if (!cur_.valid) {
      cur_.valid = true, cur_.frame = frame;
      return true;
    }
    if (frame < cur_.frame) return false;
    if (frame > cur_.frame) {
      if (closed_count_ - taken_ >= kRing) {  // the oldest closed frame is overwritten before Emit took it
        ++taken_;
        ++stats_.late;
      }
      cur_.origin = origin_, cur_.origin_known = origin_known_;  // the snapshot, at close
      ring_[closed_count_ % kRing] = std::move(cur_);
      ++closed_count_;
      cur_ = FrameBuffer();
      cur_.valid = true, cur_.frame = frame;
      index_.clear();
      ++stats_.frames;
    }
    return true;
  }

  mutable std::mutex mu_;
  FrameBuffer cur_, ring_[kRing];
  std::unordered_map<uint64_t, uint32_t> index_;
  std::unordered_map<uint32_t, std::vector<float>> first_;  // MovingStatic only
  uint64_t closed_count_ = 0, taken_ = 0;
  uint32_t origin_ = 0;
  bool origin_known_ = false, clock_first_ = false;
  float clock_frozen_ = 0.f;
  Stats stats_;
};

}  // namespace rdr_scene::live::arenaworld
