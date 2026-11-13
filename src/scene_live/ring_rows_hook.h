// The ring's per-call node rows: the game-side half (Session 101, lane 194 = Ring D3 piece R1; docs\renderer\SKIP_GAME_WORK_MAP.md 16.3 / 16.7). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only core: no game or SDK includes. The game plumbing is ring_rows_glue.h; tools\ring\ring_rows_test.cpp drives this same code with a fake guest.
//
// At every model call (the existing mid-asm hook wwe2k14_model_entry, sub_82C04DF0 at 0x82C04E1C, and on a skipped call the D3 glue's g_entry: r3 = model object, r5 = the palette the packer
// reads) it recognises ring objects by the table at [0x836F4C74] (ring_rows_core.h) and, for each ring call, copies the node rows AT THE CALL: one RingRows entry (stamped with the DRAWING
// thread's guest frame, never the record's) and N node rows in the pool. Read only. Unlike arena_world_hook.h it does NOT dedupe by (model, r5): the three heights of a rope side share one r5
// buffer that the game rewrites between heights, and the instances of one model (4 posts, 12 turnbuckles...) share the model address; every call is one entry.
// `call` = the index among ALL the model's calls of the frame (counted for every call of a ring object this hook sees, nested or not): the LightRef ordinal's rule (light_state_hook.h
// NextOrdinalLocked); the contributor cross-checks the two (CrossCheckLightRefs) and the census says whether they agree.
// Frames, ring buffer, TakeNext, CloseBefore: as arena_world_hook.h / light_state_hook.h (a call in a later frame closes the buffer; a closed frame overwritten before Emit took it is `late`).
// Capacity: kMaxCalls entries and kMaxPoolNodes pool rows a frame; a call that does not fit is counted `overflow` (never a silent truncation), a count above kRingMaxNodes is kRrCapped.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "ring_rows_core.h"

namespace rdr_scene::live::ringrows {

inline constexpr uint32_t kMaxCalls = 512;       // RingRows entries a frame (the ring makes ~44 model calls a frame)
inline constexpr uint32_t kMaxPoolNodes = 8192;  // pool rows a frame (the ring's rows total ~600-1000 a frame)
inline constexpr uint32_t kRing = 4;

// Tests only: a planted fault must fail its NAMED check (the shipped code carries each as one branch).
enum class Plant : uint32_t {
  None = 0,
  DedupeModelR5,        // the arena's key: one entry per (model, r5): ropes (and the instance calls) collapse
  LateCopy,             // the rows are read at the NEXT call of the same r5 (or the frame end), not at the call: the rewritten buffer is what is recorded
  EndlessRetry,         // a call whose rows were unreadable is retried at every later call until it reads (instead of being recorded as unreadable)
  CountFailureIsHole,   // frames the sink skipped (old calls) are counted as failures that latch
  Offset0,              // the frame stamp is the record's frame, not the hook's (the +1 offset lost)
  AlphaZero,            // alpha not read
  OrdinalByModelR5,     // `call` counted per (model, r5) instead of per model
  NoForward,            // the node count read from [this+300] only: no forward through [this+220]
};

struct Stats {
  uint64_t calls = 0, ring_calls = 0, recorded = 0, rows_copied = 0, unreadable = 0, count_unknown = 0, capped = 0, foreign_r5 = 0, forwarded = 0, overflow = 0, old_calls = 0, late = 0, frames = 0,
           frames_full = 0, table_reads = 0, table_missing = 0, alpha_unread = 0, proxy_calls = 0, retries = 0, latched = 0;
  uint64_t by_class[kRcCount] = {};
};

struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<RingRows> rows;
  std::vector<RingNodeRows> nodes;
  uint32_t group_calls[kRgCount] = {};
  void Clear() {
    frame = 0, valid = false;
    rows.clear(), nodes.clear();
    std::memset(group_calls, 0, sizeof group_calls);
  }
};

// What one call did (the glue logs the first few).
struct CallResult {
  bool ring = false, recorded = false;
  uint8_t cls = 0;
  uint32_t call = 0, n_nodes = 0, flags = 0;
};

class Tracker {
 public:
  Plant plant = Plant::None;

  // `read_word(addr, v)`; `read_mats(addr, n, std::vector<Mat4>&)` (big-endian guest bytes -> host floats, false on any fault): the fake in the test, wrestlers::ReadWord / ReadMatrices in the game.
  template <class ReadWordFn, class ReadMatsFn>
  CallResult OnModelCall(uint32_t model, uint32_t palette_ptr, uint64_t frame, ReadWordFn read_word, ReadMatsFn read_mats) {
    CallResult res;
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.calls;
    if (!Advance(frame)) { ++stats_.old_calls; if (plant == Plant::CountFailureIsHole) ++stats_.latched; return res; }
    if (!table_frame_valid_ || table_frame_ != cur_.frame) {   // one table read a frame (the first call), not a read per call
      ++stats_.table_reads;
      if (!ReadRingTable(read_word, table_)) ++stats_.table_missing;
      table_frame_ = cur_.frame, table_frame_valid_ = true;
    }
    if (plant == Plant::LateCopy && !pending_.empty()) {   // the planted bug: the rows of the PREVIOUS ring call are read now, after the game may have rewritten the shared buffer
      for (const Pending& p : pending_) {
        mats_.clear();
        if (p.frame == cur_.frame && read_mats(p.r5, p.count, mats_) && mats_.size() >= p.count)
          for (uint32_t i = 0; i < p.count; ++i) arenaworld::RowsFromMatrix(mats_[i].m, cur_.nodes[p.first + i].rows);
      }
      pending_.clear();
    }
    const uint8_t cls = table_.ClassOf(model);
    if (cls == kRcNone) return res;
    res.ring = true, res.cls = cls;
    ++stats_.ring_calls;
    ++stats_.by_class[cls];
    uint32_t call = NextCall(model, palette_ptr);
    res.call = call;
    if (cls == kRcProxy) ++stats_.proxy_calls;   // the plane-shadow proxy is recorded like the rest (the census counts it apart)
    if (plant == Plant::DedupeModelR5) {
      const uint64_t key = (uint64_t(model) << 32) | palette_ptr;
      for (uint64_t k : dedupe_)
        if (k == key) return res;
      dedupe_.push_back(key);
    }
    if (cur_.rows.size() >= kMaxCalls) { ++stats_.overflow; return res; }
    RingRows e;
    e.klass = cls;
    e.object = model;
    e.frame = uint32_t(plant == Plant::Offset0 ? frame + 1 : frame);
    e.call = call;
    e.keyed = KeyedPeek(model);   // advanced only when the entry is recorded (below)
    e.r5 = palette_ptr;
    e.first_node = uint32_t(cur_.nodes.size());
    uint32_t flags = 0, field = 0;
    if (palette_ptr != 0 && read_word(model + kPaletteOffset, field) && field != palette_ptr) flags |= kRrForeignR5;
    uint32_t aw = 0;
    if (plant == Plant::AlphaZero) {
      e.alpha = 0.f;
    } else if (read_word(model + kModelAlphaOffset, aw)) {
      std::memcpy(&e.alpha, &aw, 4);
    } else {
      flags |= kRrAlphaUnread, ++stats_.alpha_unread;
    }
    int32_t n = 0;
    bool counted = false;
    if (plant == Plant::NoForward) {   // planted: the forward through [this+220] is not followed
      uint32_t f296 = 0, f300 = 0;
      counted = read_word(model + 296, f296) && f296 != 0 && read_word(model + 300, f300);
      n = int32_t(f300);
    } else {
      counted = NodeCount(read_word, model, n, flags);
    }
    if (!counted || n <= 0) {
      flags |= kRrCountUnknown, ++stats_.count_unknown;
      e.n_nodes = 0;
    } else {
      uint32_t count = uint32_t(n);
      if (count > kRingMaxNodes) count = kRingMaxNodes, flags |= kRrCapped, ++stats_.capped;
      if (cur_.nodes.size() + count > kMaxPoolNodes) { ++stats_.overflow; return res; }
      mats_.clear();
      if (palette_ptr == 0 || !read_mats(palette_ptr, count, mats_) || mats_.size() < count) {
        flags |= kRrUnreadable, ++stats_.unreadable;
        if (plant == Plant::EndlessRetry) retry_.push_back(RetryEnt{model, palette_ptr, count});   // the planted bug: kept and retried at every later call
      } else {
        if (plant == Plant::LateCopy) pending_.push_back(Pending{cur_.frame, palette_ptr, uint32_t(cur_.nodes.size()), count});   // the planted bug: the zero rows are filled at the next ring call
        for (uint32_t i = 0; i < count; ++i) {
          RingNodeRows r{};
          if (plant != Plant::LateCopy) arenaworld::RowsFromMatrix(mats_[i].m, r.rows);
          cur_.nodes.push_back(r);
        }
        e.n_nodes = uint16_t(count);
        stats_.rows_copied += count;
      }
    }
    if (flags & kRrForeignR5) ++stats_.foreign_r5;
    if (flags & kRrCountForwarded) ++stats_.forwarded;
    e.flags = flags;
    cur_.rows.push_back(e);
    KeyedAdvance(model);
    ++cur_.group_calls[GroupOf(cls)];
    ++stats_.recorded;
    res.recorded = true, res.n_nodes = e.n_nodes, res.flags = flags;
    if (plant == Plant::EndlessRetry && !retry_.empty()) {   // every later call re-reads the failed ones (counted): the retry never ends while the buffer stays unreadable
      for (RetryEnt& r : retry_) {
        ++stats_.retries;
        mats_.clear();
        if (read_mats(r.r5, r.count, mats_)) r.count = 0;
      }
      retry_.erase(std::remove_if(retry_.begin(), retry_.end(), [](const RetryEnt& r) { return r.count == 0; }), retry_.end());
    }
    return res;
  }

  // lane 152 (review B1) rule: the record writer closes every open frame older than the present's guest frame.
  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_.valid && cur_.frame < frame) Advance(frame);
  }
  // The oldest closed frame not yet taken; false when none. Swaps the buffer out (no copy).
  bool TakeNext(FrameBuffer& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (taken_ >= closed_count_) return false;
    FrameBuffer& b = ring_[taken_ % kRing];
    std::swap(out, b);   // the caller's old buffer (its capacity) goes back into the slot
    b.Clear();
    ++taken_;
    return true;
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
  }

 private:
  struct RetryEnt { uint32_t model, r5, count; };
  struct Pending { uint64_t frame; uint32_t r5, first, count; };

  uint32_t NextCall(uint32_t model, uint32_t palette_ptr) {
    const uint32_t key = plant == Plant::OrdinalByModelR5 ? model ^ (palette_ptr * 2654435761u) : model;
    for (auto& c : calls_)
      if (c.first == key) return c.second++;
    calls_.push_back({key, 1});
    return 0;
  }
  uint32_t KeyedPeek(uint32_t model) const {
    for (const auto& c : keyed_)
      if (c.first == model) return c.second;
    return 0;
  }
  void KeyedAdvance(uint32_t model) {
    for (auto& c : keyed_)
      if (c.first == model) { ++c.second; return; }
    keyed_.push_back({model, 1});
  }
  // Returns false for a call of an already closed frame.
  bool Advance(uint64_t frame) {
    if (!cur_.valid) {
      cur_.valid = true, cur_.frame = frame;
      return true;
    }
    if (frame < cur_.frame) return false;
    if (frame > cur_.frame) {
      if (closed_count_ - taken_ >= kRing) {   // the oldest closed frame is overwritten before Emit took it
        ++taken_;
        ++stats_.late;
      }
      bool full = true;
      for (uint32_t g = 0; g < kRgOther; ++g) full = full && cur_.group_calls[g] == kExpectedCalls[g];
      if (full) ++stats_.frames_full;
      std::swap(ring_[closed_count_ % kRing], cur_);   // keeps every buffer's capacity (no reallocation of ~1000 rows under the mutex each frame)
      ++closed_count_;
      cur_.Clear();
      cur_.valid = true, cur_.frame = frame;
      calls_.clear(), keyed_.clear(), dedupe_.clear();
      ++stats_.frames;
    }
    return true;
  }

  mutable std::mutex mu_;
  FrameBuffer cur_, ring_[kRing];
  RingTable table_;
  uint64_t table_frame_ = 0;
  bool table_frame_valid_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> calls_, keyed_;
  std::vector<uint64_t> dedupe_;
  std::vector<RetryEnt> retry_;
  std::vector<Pending> pending_;
  std::vector<Mat4> mats_;
  uint64_t closed_count_ = 0, taken_ = 0;
  Stats stats_;
};

}  // namespace rdr_scene::live::ringrows
