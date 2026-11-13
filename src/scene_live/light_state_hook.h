// The per-draw light state, the game-side half (Session 101, lane 109 = LS1; docs\renderer\PER_DRAW_LIGHTS.md sections 1, 3 option (c)).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only core: no game or SDK includes. The game plumbing (guest reads, the mid-asm hook
// register arguments, the log) is light_state_glue.h; tools\light\light_states_test.cpp drives this same code with a fake guest memory.
//
// What it does. At every model call (the existing mid-asm hook wwe2k14_model_entry in sub_82C04DF0, 0x82C04E1C) it reads the calling thread's
// render camera record R and keeps, per frame, a deduplicated table of the states seen plus one (object, ordinal, state) ref per call:
//   G   = word 0x8392DDA4                  [C PER_DRAW_LIGHTS.md, listing sub_82BCBFD8]
//   idx = word [[[r13]+1048]+32], or 0 when [[r13]+1048] is null (sub_82BCBFD8 uses slot 0 then; review F1)
//   R   = word [G+4] + 2208 * idx
// It reads R's 2208 bytes once and compares the used ranges (R+0..1167 and R+2144..2207, plus the map size G+208/+212) as RAW bytes with the slot's last
// state (the common case: an object's parts and its neighbours share a state); only a different state is hashed, searched in the frame's table and, if new,
// decoded to host floats (<= kMaxStates). Read only: never writes guest memory or a register. With the switch off the glue is never called.
//
// Threads. Hooks run on game threads (the draw threads); Emit (light_state_contrib.h) runs on the writer's thread. One short mutex guards the
// buffers; the guest reads happen outside it.
// Frames. The hook is handed the guest frame it ran in. A call in a later frame than the current one closes the current buffer into a ring of
// kRing closed frames; Emit takes every closed frame not yet emitted, oldest first (TakeNext swaps the buffer out: no copy). A closed frame overwritten
// before Emit took it is counted (`late`). The LightRef carries the hook's frame, so the join to the stream's frame does not depend on the record header.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"

namespace rdr_scene::live::lights {

inline constexpr uint32_t kGlobalWord = 0x8392DDA4;  // word -> G (the render camera globals); G+4 -> the array of 2208-byte records
inline constexpr uint32_t kRecordSize = 2208;
inline constexpr uint32_t kMaxSlot = 16;             // a thread-slot index at or above this is refused (counted): the game uses a handful [L]
inline constexpr uint32_t kMaxStates = 64;           // per frame; the captures show <= 8 [C PER_DRAW_LIGHTS.md]
inline constexpr uint32_t kMaxRefs = 8192;           // per frame; ~300 model calls a frame [L]
inline constexpr uint32_t kObjTable = 8192;          // flat open-addressing table of per-object ordinals (power of two, load <= 0.5)
inline constexpr uint32_t kMaxObjects = kObjTable / 2;
inline constexpr uint32_t kRing = 4;                 // closed frames kept for Emit
inline constexpr uint32_t kUsedA = 1168, kUsedB0 = 2144, kUsedB = 64;  // the raw ranges that matter: R+0..1167, R+2144..2207
inline constexpr uint32_t kKeyBytes = kUsedA + kUsedB + 8;               // + the map size words (G+208, G+212)

// Tests only (a planted fault must fail its named check; the shipped code path carries them as one branch each, compiled in always).
enum class Plant : uint32_t { None = 0, Slot0Always, NoCompare, StaleState, StatePrevObject, ProbeOldFrame };

struct Stats {
  uint64_t calls = 0, same_as_last = 0, found = 0, appended = 0;
  uint64_t unreadable = 0, bad_slot = 0, state_overflow = 0, ref_overflow = 0, frames = 0;
  uint64_t late = 0;       // closed frames overwritten in the ring before Emit took them
  uint64_t old_calls = 0;  // calls of an already closed frame (another thread, late): not recorded
  uint64_t null_block = 0; // calls with [[r13]+1048] null: slot 0, as the game does
};

inline uint64_t HashKey(const uint8_t* p, size_t n) {  // FNV-1a 64
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// Decode the guest's big-endian record bytes (raw, as read) into a LightState (host floats). `raw` is kRecordSize bytes.
inline void DecodeRecord(const uint8_t* raw, LightState& s) {
  auto word = [&](uint32_t off) { uint32_t w; std::memcpy(&w, raw + off, 4); return __builtin_bswap32(w); };
  auto flt = [&](uint32_t off) { const uint32_t w = word(off); float f; std::memcpy(&f, &w, 4); return f; };
  std::memset(&s, 0, sizeof s);
  for (uint32_t i = 0; i < kLsLightFloats; ++i) s.light[i] = flt(4 * i);
  for (uint32_t m = 0; m < 3; ++m)
    for (uint32_t i = 0; i < 16; ++i) s.shadow_m[m][i] = flt(912 + 64 * m + 4 * i);
  for (uint32_t i = 0; i < 15; ++i) s.shadow_f[i] = flt(1104 + 4 * i);
  for (uint32_t b = 0; b < 2; ++b)
    for (uint32_t i = 0; i < 8; ++i) s.shadow_x[b][i] = flt(2144 + 32 * b + 4 * i);
}

// Two states are the same when every field the packer / binder read is bit-equal, the map size included. The slot and flags are not compared: the same
// lights read from another thread slot are one state (SCENE_RECORD.md: "Equal states (everything but slot) are stored once per frame").
inline bool SameState(const LightState& a, const LightState& b) {
  return std::memcmp(a.light, b.light, sizeof a.light) == 0 && std::memcmp(a.shadow_m, b.shadow_m, sizeof a.shadow_m) == 0 &&
         std::memcmp(a.shadow_f, b.shadow_f, sizeof a.shadow_f) == 0 && std::memcmp(a.shadow_x, b.shadow_x, sizeof a.shadow_x) == 0 &&
         std::memcmp(a.map_wh, b.map_wh, sizeof a.map_wh) == 0;
}

// One frame's worth.
struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<LightState> states;
  std::vector<LightRef> refs;
  std::vector<uint64_t> hashes;                       // per state: HashKey of its key bytes
  std::vector<std::vector<uint8_t>> keys;             // per state: the key bytes (raw ranges + map words) for the confirm after a hash hit
  void Clear() { valid = false; states.clear(), refs.clear(), hashes.clear(), keys.clear(); }  // capacity stays
};

class Tracker {
 public:
  Tracker() {
    cur_.states.reserve(kMaxStates), cur_.refs.reserve(kMaxRefs);
  }
  void SetPlant(Plant p) { plant_ = p; }
  void SetLogBudget(uint32_t n) { log_budget_ = n; }

  // One model call. `read_word(addr, uint32_t&)` and `read_bytes(addr, void*, n)` are the guest reads (false = unreadable); `log(const char*)` gets the
  // first few calls' (object, ordinal, state) lines. `frame` = the guest frame read on this thread; `r13` = the guest thread register.
  template <class ReadWordFn, class ReadBytesFn, class LogFn>
  void OnModelCall(uint32_t model, uint32_t r13, uint64_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, LogFn log) {
    uint32_t g = 0, arr = 0, tls = 0, blk = 0, idx = 0, wbits = 0, hbits = 0;
    bool null_block = false;
    if (!read_word(kGlobalWord, g) || g == 0 || !read_word(g + 4, arr) || arr == 0 || !read_word(r13, tls) || tls == 0 || !read_word(tls + 1048, blk)) {
      Count(&Stats::unreadable);
      return;
    }
    if (blk == 0) {  // sub_82BCBFD8: no recording-list block: slot 0
      idx = 0, null_block = true;
    } else if (!read_word(blk + 32, idx)) {
      Count(&Stats::unreadable);
      return;
    }
    if (plant_ == Plant::Slot0Always) idx = 0;
    if (idx >= kMaxSlot) {
      Count(&Stats::bad_slot);
      return;
    }
    uint8_t raw[kRecordSize];
    if (!read_bytes(arr + idx * kRecordSize, raw, kRecordSize)) {
      Count(&Stats::unreadable);
      return;
    }
    const bool have_map = read_word(g + 208, wbits) && read_word(g + 212, hbits);
    if (!have_map) wbits = hbits = 0;
    uint8_t key[kKeyBytes];
    std::memcpy(key, raw, kUsedA);
    std::memcpy(key + kUsedA, raw + kUsedB0, kUsedB);
    std::memcpy(key + kUsedA + kUsedB, &wbits, 4);
    std::memcpy(key + kUsedA + kUsedB + 4, &hbits, 4);
    char line[160];
    bool do_log = false;
    uint32_t state = 0, ordinal = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.calls;
      if (null_block) ++stats_.null_block;
      if (!cur_.valid || frame != cur_.frame) {
        if (cur_.valid && frame < cur_.frame) { ++stats_.old_calls; return; }  // a late call of an older frame (another thread): not recorded
        RotateLocked(frame);
      }
      Slot& sl = slot_[idx];
      bool have = false;
      if (plant_ != Plant::NoCompare && sl.valid && sl.frame == cur_.frame && sl.index < cur_.states.size() &&
          (plant_ == Plant::StaleState || std::memcmp(sl.key, key, kKeyBytes) == 0)) {  // StaleState (planted): never notices a change after the slot's first state
        state = sl.index;
        have = true;
        ++stats_.same_as_last;
      }
      uint64_t h = 0;
      if (!have && plant_ != Plant::NoCompare) {
        h = HashKey(key, kKeyBytes);
        for (uint32_t i = 0; i < cur_.states.size(); ++i)
          if (cur_.hashes[i] == h && std::memcmp(cur_.keys[i].data(), key, kKeyBytes) == 0) { state = i; have = true; ++stats_.found; break; }
      }
      if (!have) {
        if (cur_.states.size() >= kMaxStates) {
          ++stats_.state_overflow;
          return;
        }
        state = uint32_t(cur_.states.size());
        cur_.states.emplace_back();
        LightState& st = cur_.states.back();
        DecodeRecord(raw, st);
        st.slot = idx;
        if (have_map) {
          std::memcpy(&st.map_wh[0], &wbits, 4);
          std::memcpy(&st.map_wh[1], &hbits, 4);
          st.flags |= kLsMapKnown;
        }
        cur_.hashes.push_back(h ? h : HashKey(key, kKeyBytes));
        cur_.keys.emplace_back(key, key + kKeyBytes);
        ++stats_.appended;
      }
      sl.valid = true, sl.frame = cur_.frame, sl.index = state;
      std::memcpy(sl.key, key, kKeyBytes);
      if (cur_.refs.size() >= kMaxRefs) {
        ++stats_.ref_overflow;
        return;
      }
      ordinal = NextOrdinalLocked(model);
      if (ordinal == 0xFFFFFFFFu) {
        ++stats_.ref_overflow;
        return;
      }
      uint32_t ref_state = state;
      if (plant_ == Plant::StatePrevObject) ref_state = prev_state_;  // planted: each ref points at the previous call's state
      prev_state_ = state;
      cur_.refs.push_back({model, ordinal, ref_state, uint32_t(cur_.frame)});
      if (log_budget_ > 0) { --log_budget_; do_log = true; }
    }
    if (do_log) {
      std::snprintf(line, sizeof line, "[wwe2k14][lights] frame %llu: object %08X ordinal %u state %u (slot %u)", (unsigned long long)frame, model, ordinal,
                    state, idx);
      log(line);
    }
  }

  // lane 152 (D3-F item 16): the D3 decision's pre-read. Would OnModelCall record this call's state and ref, or fail? Read only: no ref, no state, no counter, no frame rotation. `table_states` = the states the
  // frame holds so far (0 when the frame has not begun here yet). The D3 hook asks it BEFORE deciding, so a skip is never taken for a call whose state cannot be written (Why::Unreadable / StateOverflow).
  enum class Probe : uint8_t { Ok, Unreadable, StateOverflow };
  template <class ReadWordFn, class ReadBytesFn>
  Probe ProbeModelCall(uint32_t model, uint32_t r13, uint64_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, uint32_t& table_states, uint32_t& ordinal) const {
    table_states = 0;
    ordinal = 0;
    uint32_t g = 0, arr = 0, tls = 0, blk = 0, idx = 0, wbits = 0, hbits = 0;
    if (!read_word(kGlobalWord, g) || g == 0 || !read_word(g + 4, arr) || arr == 0 || !read_word(r13, tls) || tls == 0 || !read_word(tls + 1048, blk)) return Probe::Unreadable;
    if (blk != 0 && !read_word(blk + 32, idx)) return Probe::Unreadable;
    if (idx >= kMaxSlot) return Probe::Unreadable;
    uint8_t raw[kRecordSize];
    if (!read_bytes(arr + idx * kRecordSize, raw, kRecordSize)) return Probe::Unreadable;
    if (!(read_word(g + 208, wbits) && read_word(g + 212, hbits))) wbits = hbits = 0;
    uint8_t key[kKeyBytes];
    std::memcpy(key, raw, kUsedA);
    std::memcpy(key + kUsedA, raw + kUsedB0, kUsedB);
    std::memcpy(key + kUsedA + kUsedB, &wbits, 4);
    std::memcpy(key + kUsedA + kUsedB + 4, &hbits, 4);
    const uint64_t h = HashKey(key, kKeyBytes);
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_.valid && frame < cur_.frame && plant_ != Plant::ProbeOldFrame) return Probe::Unreadable;   // (review S5) OnModelCall drops a call of an already closed frame (old_calls): nothing would be written
    if (!cur_.valid || frame != cur_.frame) return Probe::Ok;   // a new frame: its first state always has room; its first ordinal is 0
    ordinal = PeekOrdinalLocked(model);   // (review S6) the ordinal OnModelCall will assign this call (nested calls included)
    if (ordinal == 0xFFFFFFFFu) return Probe::StateOverflow;
    table_states = uint32_t(cur_.states.size());
    for (uint32_t i = 0; i < cur_.states.size(); ++i)
      if (cur_.hashes[i] == h && std::memcmp(cur_.keys[i].data(), key, kKeyBytes) == 0) return cur_.refs.size() >= kMaxRefs ? Probe::StateOverflow : Probe::Ok;
    return (cur_.states.size() >= kMaxStates || cur_.refs.size() >= kMaxRefs) ? Probe::StateOverflow : Probe::Ok;
  }

  // lane 152 (review B1): close every open frame older than the present's guest frame (called by Emit)
  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_.valid && cur_.frame < frame) RotateLocked(frame);
  }
  // Swaps out the oldest closed frame Emit has not taken (O(1), no copy). False = none. `out` should be reused by the caller (its buffers go back in the ring).
  bool TakeNext(FrameBuffer& out) {
    std::lock_guard<std::mutex> lock(mu_);
    int best = -1;
    for (uint32_t i = 0; i < kRing; ++i)
      if (ring_[i].buf.valid && !ring_[i].taken && (best < 0 || ring_[i].seq < ring_[best].seq)) best = int(i);
    if (best < 0) return false;
    std::swap(ring_[best].buf, out);
    ring_[best].buf.valid = false;  // `out`'s old contents (stale) sit in the ring slot until it is reused and cleared
    ring_[best].taken = true;
    return true;
  }
  Stats GetStats() const { std::lock_guard<std::mutex> lock(mu_); return stats_; }

 private:
  struct Slot { bool valid = false; uint64_t frame = 0; uint32_t index = 0; uint8_t key[kKeyBytes] = {}; };
  struct RingEnt { FrameBuffer buf; bool taken = true; uint64_t seq = 0; };
  struct ObjEnt { uint32_t key = 0, gen = 0, count = 0; };
  void Count(uint64_t Stats::*f) { std::lock_guard<std::mutex> lock(mu_); ++stats_.calls; ++(stats_.*f); }
  // The per-object call counter: a flat open-addressing table, "cleared" per frame by a generation stamp. 0xFFFFFFFF = full.
  uint32_t NextOrdinalLocked(uint32_t model) {
    uint32_t h = (model * 2654435761u) >> 8;
    for (uint32_t probe = 0; probe < kObjTable; ++probe) {
      ObjEnt& e = objs_[(h + probe) & (kObjTable - 1)];
      if (e.gen != gen_) {
        if (obj_count_ >= kMaxObjects) return 0xFFFFFFFFu;
        e.key = model, e.gen = gen_, e.count = 1, ++obj_count_;
        return 0;
      }
      if (e.key == model) return e.count++;
    }
    return 0xFFFFFFFFu;
  }
  uint32_t PeekOrdinalLocked(uint32_t model) const {
    uint32_t h = (model * 2654435761u) >> 8;
    for (uint32_t probe = 0; probe < kObjTable; ++probe) {
      const ObjEnt& e = objs_[(h + probe) & (kObjTable - 1)];
      if (e.gen != gen_) return obj_count_ >= kMaxObjects ? 0xFFFFFFFFu : 0;
      if (e.key == model) return e.count;
    }
    return 0xFFFFFFFFu;
  }
  void RotateLocked(uint64_t frame) {
    if (cur_.valid && !cur_.refs.empty()) {
      int slot = -1;  // a free ring slot (taken or never used), else the oldest untaken one (late)
      for (uint32_t i = 0; i < kRing; ++i)
        if (ring_[i].taken && (slot < 0 || ring_[i].seq < ring_[slot].seq)) slot = int(i);
      if (slot < 0) {
        slot = 0;
        for (uint32_t i = 1; i < kRing; ++i)
          if (ring_[i].seq < ring_[slot].seq) slot = int(i);
        ++stats_.late;
      }
      std::swap(ring_[slot].buf, cur_);
      ring_[slot].taken = false;
      ring_[slot].seq = ++seq_;
      ++stats_.frames;
    }
    cur_.Clear();  // keeps the capacity of whichever buffer cur_ now holds
    cur_.frame = frame;
    cur_.valid = true;
    ++gen_;
    obj_count_ = 0;
    prev_state_ = 0;
    for (Slot& s : slot_) s.valid = false;
  }
  mutable std::mutex mu_;
  Plant plant_ = Plant::None;
  uint32_t log_budget_ = 40;  // the first 40 calls log their (object, ordinal, state); then silence
  Stats stats_;
  FrameBuffer cur_;
  RingEnt ring_[kRing];
  uint64_t seq_ = 0;
  Slot slot_[kMaxSlot];
  std::vector<ObjEnt> objs_ = std::vector<ObjEnt>(kObjTable);
  uint32_t gen_ = 1, obj_count_ = 0;
  uint32_t prev_state_ = 0;
};

}  // namespace rdr_scene::live::lights
