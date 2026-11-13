// The wrestlers' sphere occluders, recorded (Session 101, lane 242 = H3 of docs\renderer\SKIP_GAME_WORK_MAP.md 18.8). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes: the guest reads are template parameters (as caster_hook.h), so tools\sphere\sphere_rows_test.cpp runs this code against a fake guest.
//
// WHAT IT RECORDS. sub_826DF8E0 is the per-frame update of a character's 'CCSO' component (db sphere_occluders_update_and_install; this = the component, called through the vtable slot 0x821EF7D0 by
// sub_826DFCC8, once per wrestler draw node). For each of its (two) spheres it writes the result into the component's own output arrays and passes them to sub_82BCC888 (the store the model binder
// sub_82BE7AC0 later uploads to VS c168-c171). The offline fit (tools\skip\sphere_check.py) proved the stream's c168-c171 ARE that result, but its inputs (the character's sphere table, the move's
// contact record, the world node matrices) are in no record, so this hook takes the OUTPUT at the exit of the function:
//   [C listing 0x826DF8E0-0x826DFCB8]  r31 = this; early outs: word 0x8392DDA4 == 0, [this+36] == 0, [this+40] <= 0 (count, 2 in practice);
//     char = [this+32]; skeleton = [char+48] (its [+192] is the world array the formula reads = the record's engine object, the pairing key of every wrestler row) [U until a live run logs both];
//     table row i = [this+44] + 52 i (words: +8 / +12 node of role A / B, -1 = the sphere is skipped, +16 key, +20 / +24 angles, +28..+36 offset, +40 radius, +44 scale, +48 blend);
//     P array = [this+48] + 16 i (xyz centre + radius), V array = [this+52] + 16 i (xyz axis + strength).
// A sphere whose table row has a node < 0 is skipped by the game (its output keeps the previous call's value): it is recorded with its bit missing from flags (kSoSphere0Ran << i) so a reader never
// takes a stale pair for a fresh one.
//
// WHEN. The hook stamps each row with the guest frame of the CALLING thread (never the record's) and keeps the LAST call per skeleton in a hook frame (a skeleton's update may run more than once a
// frame; the stream's c168-c171 of the frame's colour draws are the result of the last one before them [U until a live run: the verify arm of --wwe2k14_rdr_wrestlers_d3 compares]). Frames, the ring,
// TakeNext and CloseBefore are as caster_hook.h (lane 152 B1). Read only: nothing is written to the guest, no register changes, no call is skipped. INERT.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"

namespace rdr_scene::live::spheres {

inline constexpr uint32_t kGlobalWord = 0x8392DDA4;   // r23 of the listing: the render camera globals' word; 0 = the function returns at once (0x826DF8F4-0x826DF904)
inline constexpr uint32_t kOffReady = 36, kOffCount = 40, kOffChar = 32, kOffTable = 44, kOffP = 48, kOffV = 52;   // fields of the component [C listing]
inline constexpr uint32_t kCharSkeleton = 48;          // [char+48] = the skeleton object (its +192 is the world array)
inline constexpr uint32_t kRowBytes = 52, kRowNodeA = 8, kRowNodeB = 12;
inline constexpr size_t kMaxRows = 64;                 // skeletons a hook frame keeps (a match has 2 wrestlers + 7 other slots)
inline constexpr size_t kRing = 8;                     // closed frames kept for Emit

enum class Plant : uint32_t {
  None,
  FirstCall,       // a second call of the same skeleton in a frame does not replace the first                             -> hook.last_call
  SkeletonOff,     // the skeleton is [char+52], not [char+48]                                                                -> hook.object
  VFromP,          // V is read from the P array                                                                              -> hook.arrays
  StaleFrame,      // the row is stamped with the previous hook frame                                                         -> hook.frame_stamp
  SkippedAsRan,    // a sphere whose node is -1 (skipped by the game) is marked as run                                         -> hook.skipped_sphere
  NoReadyCheck,    // the early outs ([this+36] == 0, count <= 0, global word == 0) are ignored                                -> hook.early_outs
};

struct Stats {
  uint64_t calls = 0, recorded = 0, early_out = 0, unreadable = 0, bad_count = 0, no_skeleton = 0, replaced = 0, late = 0, skipped_sphere = 0, overflow = 0, frames = 0;
};

struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<SphereOccRow> rows;
  void Clear() { frame = 0, valid = false, rows.clear(); }
};

// One decode. `read_word(addr, uint32_t&)` and `read_bytes(addr, void*, n)` return false on a fault; guest bytes are big-endian. Returns false when the call is not a recordable update (the early outs:
// the game did nothing; or an unreadable field).
enum class Decode : uint8_t { Ok, EarlyOut, Unreadable, BadCount };
template <class ReadWordFn, class ReadBytesFn>
inline Decode DecodeComponent(ReadWordFn read_word, ReadBytesFn read_bytes, uint32_t comp, uint32_t frame, SphereOccRow& row, Plant plant, Stats* st = nullptr) {
  row = SphereOccRow{};
  uint32_t g = 0, ready = 0, count = 0;
  if (!read_word(kGlobalWord, g) || !read_word(comp + kOffReady, ready) || !read_word(comp + kOffCount, count)) return Decode::Unreadable;
  if (plant != Plant::NoReadyCheck && (g == 0 || ready == 0 || int32_t(count) <= 0)) return Decode::EarlyOut;
  if (int32_t(count) <= 0) return Decode::EarlyOut;
  if (count > kSphereMax) return Decode::BadCount;
  uint32_t ch = 0, tab = 0, pa = 0, va = 0;
  if (!read_word(comp + kOffChar, ch) || !read_word(comp + kOffTable, tab) || !read_word(comp + kOffP, pa) || !read_word(comp + kOffV, va) || tab == 0 || pa == 0 || va == 0) return Decode::Unreadable;
  row.component = comp, row.character = ch, row.count = count, row.frame = plant == Plant::StaleFrame ? frame - 1 : frame;
  uint32_t skel = 0;
  if (ch != 0 && read_word(ch + (plant == Plant::SkeletonOff ? kCharSkeleton + 4 : kCharSkeleton), skel) && skel != 0) {
    row.object = skel;
    row.flags |= kSoSkeleton;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t raw[16];
    uint32_t trow[kSphereRecWords];
    bool tab_ok = true;
    for (uint32_t w = 0; w < kSphereRecWords; ++w) {
      if (!read_word(tab + kRowBytes * i + 4 * w, trow[w])) { tab_ok = false; break; }
    }
    if (!tab_ok) return Decode::Unreadable;
    std::memcpy(row.table[i], trow, sizeof trow);
    if (!read_bytes((plant == Plant::VFromP ? pa : va) + 16 * i, raw, 16)) return Decode::Unreadable;
    for (int k = 0; k < 4; ++k) {
      const uint32_t w = (uint32_t(raw[4 * k]) << 24) | (uint32_t(raw[4 * k + 1]) << 16) | (uint32_t(raw[4 * k + 2]) << 8) | raw[4 * k + 3];
      std::memcpy(&row.v[i][k], &w, 4);
    }
    if (!read_bytes(pa + 16 * i, raw, 16)) return Decode::Unreadable;
    for (int k = 0; k < 4; ++k) {
      const uint32_t w = (uint32_t(raw[4 * k]) << 24) | (uint32_t(raw[4 * k + 1]) << 16) | (uint32_t(raw[4 * k + 2]) << 8) | raw[4 * k + 3];
      std::memcpy(&row.p[i][k], &w, 4);
    }
    // The game's skips: `if (node < 0) goto next` (0x826DF97C-0x826DF98C) and `if (node*64 + [skel+192] == 0) goto next` for A and B (the beq after the add, 0x826DF9A8-0x826DF9B0).
    bool skipped = int32_t(trow[kRowNodeA / 4]) < 0 || int32_t(trow[kRowNodeB / 4]) < 0;
    if (!skipped && (row.flags & kSoSkeleton)) {
      uint32_t world = 0;
      if (read_word(row.object + 192, world)) skipped = uint32_t(trow[kRowNodeA / 4] << 6) + world == 0 || uint32_t(trow[kRowNodeB / 4] << 6) + world == 0;
    }
    if (!skipped || plant == Plant::SkippedAsRan) row.flags |= (i == 0 ? kSoSphere0Ran : kSoSphere1Ran);
    else if (st) ++st->skipped_sphere;
  }
  row.flags |= kSoSpheres | kSoTable;
  return Decode::Ok;
}

class Tracker {
 public:
  Plant plant = Plant::None;
  Tracker() { cur_.rows.reserve(kMaxRows); }

  // The hook: `frame` = the calling thread's guest frame. One row per skeleton per frame, the last call wins.
  template <class ReadWordFn, class ReadBytesFn>
  bool OnComponent(uint32_t comp, uint64_t frame, ReadWordFn read_word, ReadBytesFn read_bytes) {
    SphereOccRow row;
    Stats local;
    const Decode d = DecodeComponent(read_word, read_bytes, comp, uint32_t(frame), row, plant, &local);
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.calls;
    stats_.skipped_sphere += local.skipped_sphere;
    if (d == Decode::EarlyOut) { ++stats_.early_out; return false; }
    if (d == Decode::BadCount) { ++stats_.bad_count; return false; }
    if (d != Decode::Ok) { ++stats_.unreadable; return false; }
    if (!Advance(frame)) { ++stats_.late; return false; }   // a call of an already closed frame
    if (!(row.flags & kSoSkeleton)) ++stats_.no_skeleton;
    else if (row.object) known_.insert(row.object);   // lane 245: a skeleton that has spheres
    row.calls = 1;
    for (SphereOccRow& r : cur_.rows) {
      if (r.object == row.object && r.component == row.component) {   // the same skeleton and component again this frame
        ++stats_.replaced;
        const uint32_t calls = r.calls + 1;
        if (plant != Plant::FirstCall) r = row;
        r.calls = calls;
        return true;
      }
    }
    if (cur_.rows.size() >= kMaxRows) { ++stats_.overflow; return false; }
    cur_.rows.push_back(row);
    ++stats_.recorded;
    return true;
  }

  // lane 245 (SKIP_GAME_WORK_MAP.md 18.9, reviewer SF2): the wrestler hook's question before it skips an object. 0 = the object was never seen as a sphere skeleton (it has no sphere registers: nothing to ask), 1 = a row of it
  // is in the open buffer of `frame` (the update ran this hook frame), 2 = a known sphere skeleton with no row in that frame (the marker's c168-c171 would have no source: the skip is refused). Read only.
  int RowState(uint64_t frame, uint32_t object) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!known_.count(object)) return 0;
    if (cur_.valid && cur_.frame == frame)
      for (const SphereOccRow& r : cur_.rows)
        if (r.object == object) return 1;
    return 2;
  }

  // lane 152 (review B1) rule: the record writer closes every open frame older than the present's guest frame.
  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_.valid && cur_.frame < frame) Advance(frame);
  }
  bool TakeNext(FrameBuffer& out) {   // the oldest closed frame not yet taken; false when none (swaps, no copy)
    std::lock_guard<std::mutex> lk(mu_);
    if (taken_ >= closed_count_) return false;
    FrameBuffer& b = ring_[taken_ % kRing];
    std::swap(out, b);
    b.Clear();
    ++taken_;
    return true;
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
  }

 private:
  bool Advance(uint64_t frame) {   // false for a call of an already closed frame; mu_ held
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
      std::swap(ring_[closed_count_ % kRing], cur_);
      ++closed_count_;
      cur_.Clear();
      cur_.valid = true, cur_.frame = frame;
      ++stats_.frames;
    }
    return true;
  }

  mutable std::mutex mu_;
  FrameBuffer cur_, ring_[kRing];
  uint64_t closed_count_ = 0, taken_ = 0;
  Stats stats_;
  std::unordered_set<uint32_t> known_;   // lane 245: the skeletons a row was ever recorded for (bounded: a match has a handful)
};

}  // namespace rdr_scene::live::spheres
