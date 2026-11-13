// The caster passes' per-entry record: the game-side tracker (Session 101, lane 202 = Caster D3 pieces E0-E2; docs\renderer\SKIP_GAME_WORK_MAP.md 17.3 / 17.7). Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause. Header-only core: no game or SDK includes. The game plumbing is caster_glue.h; tools\caster\caster_rows_test.cpp drives this same code with a fake guest.
//
// At every entry of a caster pass (REX_HOOK_RAW on sub_82C01FE0 / sub_82BF22C8 / sub_82BF9878 in draw_origin.h: r3 = model, r4 = pass key, r5 = palette pointer or 0, r13 = thread) it keeps one
// CasterCall per entry: the render camera R as read AT THE ENTRY (deduplicated per frame like the light states), the pass key, the palette pointer and count, a hash of the palette's matrices,
// the map-A collapse flag and [model+408..416]. Read only: nothing is written to the guest, no register changes, no call is skipped (caster_core.h kCasterSkipBuilt is false).
// Frames, ring buffer, TakeNext, CloseBefore: as ring_rows_hook.h / light_state_hook.h (the entry carries the DRAWING thread's guest frame, never the record's; an entry of an already closed frame is
// counted `old_calls` and is NOT a failure; a closed frame overwritten before Emit took it is `late`). The ordinal is per (pass, object): "three passes: three ordinal spaces" (17.5).
// Everything the entry reads is read BEFORE the mutex (guest reads outside it), exactly once: an unreadable value is recorded as a flag, never retried.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "caster_core.h"

namespace rdr_scene::live::casters {

// Tests only: a planted fault must fail its NAMED check (the shipped code carries each as one branch).
enum class Plant : uint32_t {
  None = 0,
  LateRecord,          // (test harness only: the consumer sees record R_f one present late, lane 152 B1) -> test.live_order_join; the tracker carries no branch for it
  EndlessRetry,        // a call whose palette was unreadable is retried at every later call until it reads -> test.unreadable_once
  CountFailureIsHole,  // calls of an already closed frame (the sink skipped them) are counted as failures that latch -> test.old_calls_not_failures
  Offset0,             // the stamp is the record's frame, not the hook's (the +1 offset lost) -> test.live_order_join
  OrdinalMerge,        // the ordinal ignores the pass (map A / B, plane and rim share one counter per object) -> test.ordinals
  StaleSlots,          // the hash covers the model's whole palette array, not the count (slots past the part's own bones are counted) -> test.palette_hash
  CameraPrevFrame,     // the camera state is the slot's state of the previous frame (a cache that is not invalidated at the frame boundary) -> test.camera_per_entry
  LateRead,            // R is read when the NEXT entry arrives, not at the entry -> test.camera_per_entry
  MapAKeyIgnored,      // the collapse flag is not read -> test.collapse
  ProductSlot0,        // lane 221: the camera product is read for slot 0, not the calling thread's slot -> hook.extra_per_entry
  ProductPrevFrame,    // lane 221: the camera product of the FIRST entry of the previous frame is reused (a cache never invalidated at the frame boundary) -> hook.extra_per_entry
  PoseRowOff,          // lane 221: the collapse point is row 2 of the pose matrix, not row 3 -> hook.pose_point
  NodeShift,           // lane 217: the per-node hashes are stored one node early (node k holds the hash of node k + 1) -> hook.node_located
  PlaneR5Palette,      // lane 226 (E5): a plane entry takes r5 as the palette pointer (lanes 202-221's rule; the plane pass never reads r5, it always reads [model+200]) -> hook.plane_mats
  PlaneMatsShift,      // lane 226: the plane matrices are stored one node early (node k holds node k + 1) -> hook.plane_mats
  PlanePosDropped,     // lane 226 follow-up (S1): the plane entry's c7 snapshot (plane_pos) is not stored in the extra -> hook.plane_pos
};

struct Stats {
  uint64_t calls = 0, recorded = 0, unreadable = 0, bad_slot = 0, no_state = 0, state_overflow = 0, overflow = 0, old_calls = 0, late = 0, frames = 0, collapse_unread = 0, retries = 0, latched = 0;
  uint64_t no_palette = 0, pal_unreadable = 0, capped = 0, foreign_r5 = 0, r5_zero = 0, appended = 0, found = 0, same_as_last = 0, collapsed = 0;
  uint64_t node_hashes = 0;   // lane 217: per-node hashes recorded
  uint64_t plane_mats = 0, plane_calls_mats = 0, plane_mats_unreadable = 0;   // lane 226 (E5): palette matrices recorded for plane entries / plane entries that carry them / plane entries whose [model+200] could not be read
  uint64_t extra_product = 0, extra_depth = 0, extra_pose = 0, extra_unreadable = 0;   // lane 221: CasterExtra fields read / entries whose product could not be read
  uint64_t by_pass[kCpCount] = {};
};

struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<CasterCall> calls;
  std::vector<LightState> states;
  std::vector<uint64_t> hashes;
  std::vector<CasterExtra> extras;   // lane 221: parallel to `calls`
  std::vector<uint32_t> nodes;   // lane 217: the per-node palette hashes the calls' node_first index (bounded: kMaxCalls * kCasterMaxNodes)
  std::vector<Mat4> mats;        // lane 226 (E5): the plane entries' palette matrices (kCcMats calls' node_first index it; bounded: kMaxCalls * kCasterMaxNodes)
  std::vector<std::vector<uint8_t>> keys;
  uint32_t pass_calls[kCpCount] = {};
  void Clear() {
    frame = 0, valid = false;
    calls.clear(), states.clear(), hashes.clear(), nodes.clear(), mats.clear(), keys.clear(), extras.clear();
    std::memset(pass_calls, 0, sizeof pass_calls);
  }
};

// What one entry did (the glue logs the first few).
struct CallResult {
  bool recorded = false;
  uint32_t ordinal = 0, state = kCasterNoState, n_nodes = 0, flags = 0;
  uint8_t collapse = 0;
};

class Tracker {
 public:
  Plant plant = Plant::None;
  Tracker() {
    cur_.calls.reserve(kMaxCalls), cur_.states.reserve(kMaxStates), cur_.extras.reserve(kMaxCalls);
  }

  // `read_word(addr, v)`, `read_bytes(addr, dst, n)`, `read_mats(addr, n, std::vector<Mat4>&)` (big-endian guest bytes -> host floats, false on any fault), `want_count(model)` = the wrestler contributor's
  // WantCount (0 = not a skeleton it knows). `plane_pos` (lane 226 follow-up, S1): the plane tracker's c7 at this entry (null = not written this frame); only a plane entry stores it (CasterExtra::pose_t, kCxPlanePos).
  // For a PLANE entry `key` (r4) and `r5` are junk: sub_82BF22C8's only input is r3 [C listing 0x82BF22C8-0x82BF22EC: r3 is saved to r26; r4 is overwritten by `addi r4,r1,144` at 0x82BF22E0 before it is read; r5 is never read].
  template <class ReadWordFn, class ReadBytesFn, class ReadMatsFn, class WantFn>
  CallResult OnPassEntry(uint8_t pass, uint32_t model, uint32_t key, uint32_t r5, uint32_t r13, uint64_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, ReadMatsFn read_mats, WantFn want_count, const float* plane_pos = nullptr) {
    CallResult res;
    if (pass == kCpNone || pass >= kCpCount) return res;
    if (plant == Plant::LateRead && have_pending_) FlushPending(read_word, read_bytes);
    // ---- the guest reads, once, outside the lock ----
    EntryR ler;   // a local: the three passes may run on different game threads, so nothing read outside the lock is shared
    EntryR* er = &ler;
    thread_local std::vector<Mat4> mats;
    const RStatus rs = ReadEntryR(read_word, read_bytes, r13, *er);
    uint32_t flags = er->null_block ? kCcNullBlock : 0;
    uint32_t palette = r5, field = 0;
    const bool field_ok = read_word(model + kPaletteOffset, field);
    // lane 226 (E5): the PLANE pass has no palette argument [C listing 0x82BF22C8-0x82BF22E4: the function's ONLY input is r3 (r4 is overwritten at 0x82BF22E0, r5 never read); the palette is `lwz r10,200(r26)` at 0x82BF459C, node rows `[model+200] + 64 * (node - 1)`
    // at 0x82BF45A4-0x82BF45B4, as the colour pass]: r5 is whatever the caller left in it (run 7149: 0x2b6, 0x33a, .. for the ring proxy, a pointer for the wrestler proxy), so the palette is always [model+200]
    // and the entry's flags say neither r5-zero nor foreign.
    const bool plane_rows = pass == kCpPlane && plant != Plant::PlaneR5Palette;
    if (plane_rows) palette = field_ok ? field : 0;
    else if (r5 == 0) flags |= kCcR5Zero, palette = field_ok ? field : 0;
    else if (field_ok && field != r5) flags |= kCcForeignR5;
    uint32_t n = pass == kCpPlane ? kPlaneNodeWindow : want_count(model);   // the plane proxies are in no table: the first kPlaneNodeWindow nodes of the buffer (a slot's node is learned by the binder, bit for bit)
    uint64_t pal_hash = 0;
    uint32_t hashed = 0;
    thread_local std::vector<uint32_t> node_hashes;   // lane 217: one hash per node, as read
    node_hashes.clear();
    thread_local std::vector<Mat4> plane_keep;   // lane 226: the plane entry's matrices, kept for the record
    plane_keep.clear();
    if (n == 0) {
      flags |= kCcNoPalette;
    } else if (palette == 0) {
      flags |= kCcUnreadable;
    } else {
      uint32_t take = plant == Plant::StaleSlots ? std::max<uint32_t>(n, 20u) : n;   // planted: slots past the part's own bones are counted as rows
      if (take > kCasterMaxNodes) take = kCasterMaxNodes, flags |= kCcCapped;
      mats.clear();
      if (!read_mats(palette, take, mats) || mats.size() < take) {
        flags |= kCcUnreadable;
      } else {
        pal_hash = HashPalette(mats.data(), take);
        hashed = take;
        if (pass == kCpPlane) {
          plane_keep.assign(mats.begin(), mats.begin() + take);
          if (plant == Plant::PlaneMatsShift && take > 1) std::rotate(plane_keep.begin(), plane_keep.begin() + 1, plane_keep.end());
        } else {
          HashNodes(mats.data(), std::min<uint32_t>(take, std::min<uint32_t>(n, kCasterMaxNodes)), node_hashes);
        }
        if (plant == Plant::NodeShift && node_hashes.size() > 1) std::rotate(node_hashes.begin(), node_hashes.begin() + 1, node_hashes.end());   // the part's own nodes only (a planted stale_slots hash covers more than the part has)
      }
    }
    uint32_t cflag = 0;
    uint8_t collapse = 0;
    uint32_t map_a[3] = {};
    bool map_a_ok = true;
    if (pass == kCpMap) {
      if (rs == RStatus::Ok && plant != Plant::MapAKeyIgnored) {
        if (ReadCollapse(read_word, er->slot, cflag)) collapse = cflag != 0 ? 1 : 0;
        else flags |= kCcCollapseUnread;
      } else if (rs == RStatus::Ok) {
        collapse = 0;
      } else {
        flags |= kCcCollapseUnread;
      }
      for (uint32_t k = 0; k < 3; ++k)
        if (!read_word(model + kMapAOffset + 4 * k, map_a[k])) map_a[k] = 0xFFFFFFFFu, map_a_ok = false;   // an unreadable word is -1 (no collapse), never 0
    }
    // lane 221 (17.10): the pass entry's reads BESIDES R: the current camera's product, RimLightDepth, the map-A collapse point. Each read once, outside the lock; an unreadable one is a missing flag.
    CasterExtra ex;
    {
      uint32_t slot = er->slot;   // the slot ReadEntryR already read (null block: 0, as the game does)
      thread_local std::vector<Mat4> pm;
      if (rs == RStatus::Ok && slot < kProductSlots) {   // the game's product table holds kProductSlots entries (0x839299C0 .. 0x83929A40, the camera pointer table follows): a larger slot is unreadable, not a guess
        pm.clear();
        if (plant == Plant::ProductSlot0) slot = 0;
        if (read_mats(kCameraProductBase + 64u * slot, 1, pm) && pm.size() >= 1) {
          std::memcpy(ex.product, pm[0].m, sizeof ex.product);
          ex.flags |= kCxProduct;
        }
      }
      uint32_t g = 0, d0 = 0, d1 = 0;
      if (read_word(lights::kGlobalWord, g) && g != 0 && read_word(g + kDepthOffset0, d0) && read_word(g + kDepthOffset1, d1)) {
        std::memcpy(&ex.depth[0], &d0, 4), std::memcpy(&ex.depth[1], &d1, 4);
        ex.flags |= kCxDepth;
      }
      if (pass == kCpMap && collapse && map_a_ok && int32_t(map_a[0]) >= 0) {   // the collapse branch runs for a map-A call only; [model+408] < 0 = no collapse (0x82C03D7C-0x82C03D84)
        uint32_t base = 0;
        uint8_t raw[16];
        const uint32_t row = plant == Plant::PoseRowOff ? kPoseRowOffset - 16 : kPoseRowOffset;
        if (read_word(model + kPoseArrayOffset, base) && base != 0 && read_bytes(base + 64u * map_a[0] + row, raw, 16)) {
          BeFloat4(raw, ex.pose_t);
          ex.flags |= kCxPose;
        }
      }
    }
    // ---- the buffer ----
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.calls;
    if (!Advance(frame)) {
      ++stats_.old_calls;
      if (plant == Plant::CountFailureIsHole) ++stats_.latched;
      return res;
    }
    if (cur_.calls.size() >= kMaxCalls) { ++stats_.overflow; return res; }
    CasterCall c;
    c.pass = pass;
    c.collapse = collapse;
    c.object = model;
    c.frame = uint32_t(plant == Plant::Offset0 ? frame + 1 : frame);
    c.ordinal = NextOrdinal(pass, model);
    c.key = key;
    c.r5 = r5;
    c.palette = palette;
    c.flags = flags;
    c.map_a[0] = map_a[0], c.map_a[1] = map_a[1], c.map_a[2] = map_a[2];
    if (hashed) {
      c.pal_hash = pal_hash, c.n_nodes = uint16_t(std::min<uint32_t>(n, kCasterMaxNodes));
      if (node_hashes.size() == c.n_nodes) {
        c.flags |= kCcNodeHashes, c.node_first = uint32_t(cur_.nodes.size());
        cur_.nodes.insert(cur_.nodes.end(), node_hashes.begin(), node_hashes.end());
        stats_.node_hashes += node_hashes.size();
      }
      if (pass == kCpPlane && plane_keep.size() == c.n_nodes && c.n_nodes > 0) {   // lane 226 (E5)
        c.flags |= kCcMats, c.node_first = uint32_t(cur_.mats.size());
        cur_.mats.insert(cur_.mats.end(), plane_keep.begin(), plane_keep.end());
        stats_.plane_mats += plane_keep.size(), ++stats_.plane_calls_mats;
      }
    }
    if (pass == kCpPlane && (flags & kCcUnreadable)) ++stats_.plane_mats_unreadable;
    if (flags & kCcNoPalette) ++stats_.no_palette;
    if (flags & kCcCapped) ++stats_.capped;
    if (flags & kCcForeignR5) ++stats_.foreign_r5;
    if (flags & kCcR5Zero) ++stats_.r5_zero;
    if ((flags & kCcUnreadable) && n != 0) {
      ++stats_.pal_unreadable;
      if (plant == Plant::EndlessRetry) retry_.push_back(Retry{palette, n < kCasterMaxNodes ? n : kCasterMaxNodes});
    }
    if (flags & kCcCollapseUnread) ++stats_.collapse_unread;
    if (collapse) ++stats_.collapsed;
    // the camera state
    if (rs != RStatus::Ok) {
      c.flags |= kCcNoState;
      rs == RStatus::BadSlot ? ++stats_.bad_slot : ++stats_.unreadable;
      ++stats_.no_state;
    } else if (plant == Plant::LateRead) {
      pending_ = Pending{true, uint32_t(cur_.calls.size()), model, r13, frame};
      have_pending_ = true;
      c.state = kCasterNoState;
    } else {
      c.state = StateFor(*er, pass, c.flags);
    }
    if (plant == Plant::ProductPrevFrame) {   // planted: the first entry's product of ANY earlier frame stays (never invalidated at the frame boundary)
      if (stale_product_valid_) std::memcpy(ex.product, stale_product_, sizeof ex.product), ex.flags |= kCxProduct;
      else if (ex.flags & kCxProduct) std::memcpy(stale_product_, ex.product, sizeof stale_product_), stale_product_valid_ = true;
    }
    if (ex.flags & kCxProduct) ++stats_.extra_product; else ++stats_.extra_unreadable;
    if (ex.flags & kCxDepth) ++stats_.extra_depth;
    if (ex.flags & kCxPose) ++stats_.extra_pose;
    if (pass == kCpPlane && plane_pos && plant != Plant::PlanePosDropped) std::memcpy(ex.pose_t, plane_pos, sizeof ex.pose_t), ex.flags |= kCxPlanePos;   // lane 226 follow-up (S1)
    cur_.extras.push_back(ex);
    cur_.calls.push_back(c);
    ++cur_.pass_calls[pass];
    ++stats_.recorded;
    ++stats_.by_pass[pass];
    res.recorded = true, res.ordinal = c.ordinal, res.state = c.state, res.n_nodes = c.n_nodes, res.flags = c.flags, res.collapse = c.collapse;
    if (plant == Plant::EndlessRetry && !retry_.empty()) {   // every later call re-reads the failed ones (counted): the retry never ends while the buffer stays unreadable
      for (Retry& r : retry_) {
        ++stats_.retries;
        mats2_.clear();
        if (read_mats(r.palette, r.count, mats2_)) r.count = 0;
      }
      retry_.erase(std::remove_if(retry_.begin(), retry_.end(), [](const Retry& r) { return r.count == 0; }), retry_.end());
    }
    return res;
  }

  // lane 245 (Wrestlers D3 live seam, SKIP_GAME_WORK_MAP.md 18.9 / 18.7 precondition 7): does the frame being recorded (`frame` = the hook's stamp, the open buffer only) already hold the two entries a wrestler colour call's
  // marker is composed from? have_camera = a MAP entry with a palette (n_nodes > 0) and a camera state (d3_record.h RecordIndex::Add files exactly these as the wrestler light camera, wcams_); have_rim = a RIM entry whose
  // CasterExtra carries the depth range (kCxDepth; rdepth_). A frame whose entries are not (yet) here answers false, false: the hook does not skip the call. Read only.
  void FrameHasWrestlerRecords(uint64_t frame, bool& have_camera, bool& have_rim) const {
    std::lock_guard<std::mutex> lk(mu_);
    have_camera = have_rim = false;
    if (!cur_.valid || cur_.frame != frame) return;
    for (size_t i = 0; i < cur_.calls.size(); ++i) {
      const CasterCall& c = cur_.calls[i];
      if (c.pass == kCpMap && c.n_nodes > 0 && c.state != kCasterNoState) have_camera = true;
      if (c.pass == kCpRim && i < cur_.extras.size() && (cur_.extras[i].flags & kCxDepth)) have_rim = true;
    }
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
    std::swap(out, b);
    b.Clear();
    ++taken_;
    return true;
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
  }
  // The frame's expected set (the verify census: calls per pass of closed frames, for the log line).
  uint64_t FramesWithAllPasses() const {
    std::lock_guard<std::mutex> lk(mu_);
    return frames_all_;
  }

 private:
  struct Retry { uint32_t palette, count; };
  struct Pending { bool on; uint32_t index, model, r13; uint64_t frame; };
  struct Slot { bool valid = false; uint64_t frame = 0; uint32_t index = 0; uint8_t key[lights::kKeyBytes] = {}; };

  uint32_t NextOrdinal(uint8_t pass, uint32_t model) {
    const uint64_t k = plant == Plant::OrdinalMerge ? uint64_t(model) : (uint64_t(pass) << 32) | model;
    for (auto& c : ordinals_)
      if (c.first == k) return c.second++;
    ordinals_.push_back({k, 1});
    return 0;
  }
  // The state of an entry's R: the slot's last state when the used bytes are equal, else found by hash, else appended (kCcNoState when the table is full).
  uint32_t StateFor(const EntryR& e, uint8_t /*pass*/, uint32_t& flags) {
    Slot& sl = slot_[e.slot];
    if (plant == Plant::CameraPrevFrame) {   // planted: the slot's state of ANY earlier frame is reused without a compare (never invalidated at the frame boundary)
      if (sl.valid && sl.index < cur_.states.size()) { ++stats_.same_as_last; return sl.index; }
    }
    if (sl.valid && sl.frame == cur_.frame && sl.index < cur_.states.size() && std::memcmp(sl.key, e.key, lights::kKeyBytes) == 0) {
      ++stats_.same_as_last;
      return sl.index;
    }
    const uint64_t h = lights::HashKey(e.key, lights::kKeyBytes);
    for (uint32_t i = 0; i < cur_.states.size(); ++i)
      if (cur_.hashes[i] == h && std::memcmp(cur_.keys[i].data(), e.key, lights::kKeyBytes) == 0) {
        ++stats_.found;
        sl.valid = true, sl.frame = cur_.frame, sl.index = i;
        std::memcpy(sl.key, e.key, lights::kKeyBytes);
        return i;
      }
    if (cur_.states.size() >= kMaxStates) {
      ++stats_.state_overflow;
      flags |= kCcNoState;
      return kCasterNoState;
    }
    const uint32_t idx = uint32_t(cur_.states.size());
    cur_.states.emplace_back();
    LightState& st = cur_.states.back();
    lights::DecodeRecord(e.raw, st);
    st.slot = e.slot;
    if (e.have_map) {
      std::memcpy(&st.map_wh[0], &e.wbits, 4);
      std::memcpy(&st.map_wh[1], &e.hbits, 4);
      st.flags |= kLsMapKnown;
    }
    cur_.hashes.push_back(h);
    cur_.keys.emplace_back(e.key, e.key + lights::kKeyBytes);
    ++stats_.appended;
    sl.valid = true, sl.frame = cur_.frame, sl.index = idx;
    std::memcpy(sl.key, e.key, lights::kKeyBytes);
    return idx;
  }
  template <class ReadWordFn, class ReadBytesFn>
  void FlushPending(ReadWordFn read_word, ReadBytesFn read_bytes) {   // the planted LateRead: R read now, for the PREVIOUS entry
    std::lock_guard<std::mutex> lk(mu_);
    have_pending_ = false;
    if (!pending_.on || !cur_.valid || cur_.frame != pending_.frame || pending_.index >= cur_.calls.size()) return;
    EntryR e;
    if (ReadEntryR(read_word, read_bytes, pending_.r13, e) != RStatus::Ok) return;
    uint32_t fl = 0;
    cur_.calls[pending_.index].state = StateFor(e, cur_.calls[pending_.index].pass, fl);
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
      if (cur_.pass_calls[kCpMap] && cur_.pass_calls[kCpRim]) ++frames_all_;
      std::swap(ring_[closed_count_ % kRing], cur_);   // keeps every buffer's capacity
      ++closed_count_;
      cur_.Clear();
      cur_.valid = true, cur_.frame = frame;
      ordinals_.clear();
      if (plant != Plant::ProductPrevFrame) stale_product_valid_ = false;
      for (Slot& s : slot_) s.valid = false;
      have_pending_ = false;
      ++stats_.frames;
    }
    return true;
  }

  mutable std::mutex mu_;
  FrameBuffer cur_, ring_[kRing];
  std::vector<std::pair<uint64_t, uint32_t>> ordinals_;
  std::vector<Mat4> mats2_;
  std::vector<Retry> retry_;
  float stale_product_[16] = {};
  bool stale_product_valid_ = false;
  Pending pending_{};
  bool have_pending_ = false;
  Slot slot_[lights::kMaxSlot];
  uint64_t closed_count_ = 0, taken_ = 0, frames_all_ = 0;
  Stats stats_;
};

}  // namespace rdr_scene::live::casters
