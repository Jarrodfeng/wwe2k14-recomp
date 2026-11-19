// Builds a Frame from calls. State tables are de-duplicated as they are filled. All storage is reused from frame to frame.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "api.h"
#include "../renderer_scene/crowd_fast.h"  // S103 P7 tween: FlatU64Map for the own blend occurrence counts
#include "colour_cuts.h"  // S102 G10
#include "arena_fragments.h"  // S102 R18-B3
#include "frame_switches.h"
#include "group_stamps.h"  // S102 R24-F
#include "output_switches.h"  // S102 G2  // S102 T4 / T5

namespace rdr {

// S103 P7 tween (review F8): the top bit of a draw's 64-bit blend key says WHO keyed it: 1 = an own draw (FrameBuilder::TagBlend: the host, the scene front-end), 0 = the stream binder (BlendRanges). The two can never collide.
constexpr uint64_t kOwnBlendKeyBit = 1ull << 63;

// S103 PVR: every Begin gets a new, process-unique epoch (a frame's own geometry blob is valid only inside it): a front-end that caches a ring offset asks Epoch() before reusing it.
inline std::atomic<uint64_t> g_fb_epoch_seq{0};

// S103 PF2: the frame's geometry blob. Like std::vector<uint8_t> except growth does NOT zero the new bytes (vector::resize value-initialises: a zero fill of every posed block before the copy
// overwrote it, ~10 MB a frame on the Surface, docs\research\s103\RCP.md section 1). Capacity is kept by clear(). Only the alignment gap in front of a block is zeroed (the blob stays deterministic).
class GeometryBlob {
 public:
  GeometryBlob() = default;
  GeometryBlob(const GeometryBlob& o) { Assign(o); }
  GeometryBlob& operator=(const GeometryBlob& o) { if (this != &o) Assign(o); return *this; }
  GeometryBlob(GeometryBlob&& o) noexcept : data_(std::move(o.data_)), size_(o.size_), cap_(o.cap_) { o.size_ = o.cap_ = 0; }
  GeometryBlob& operator=(GeometryBlob&& o) noexcept { data_ = std::move(o.data_), size_ = o.size_, cap_ = o.cap_, o.size_ = o.cap_ = 0; return *this; }
  void clear() { size_ = 0; }
  size_t size() const { return size_; }
  const uint8_t* data() const { return data_.get(); }
  // Reserves `bytes` aligned to `align` (a power of two) and returns the pointer; `offset` is the block's start. The block's own bytes are UNINITIALISED: the caller fills all of them.
  uint8_t* Reserve(uint32_t bytes, uint32_t align, uint32_t& offset) {
    const size_t at = (size_ + align - 1) & ~size_t(align - 1);
    const size_t end = at + bytes;
    if (end > cap_) Grow(end);
    if (at > size_) std::memset(data_.get() + size_, 0, at - size_);
    size_ = end;
    offset = uint32_t(at);
    return data_.get() + at;
  }

 private:
  void Grow(size_t need) {
    size_t cap = std::max<size_t>(cap_ + cap_ / 2, need);
    cap = (cap + 4095) & ~size_t(4095);
    std::unique_ptr<uint8_t[]> next(new uint8_t[cap]);
    if (size_) std::memcpy(next.get(), data_.get(), size_);
    data_ = std::move(next), cap_ = cap;
  }
  void Assign(const GeometryBlob& o) {
    size_ = 0;
    if (o.size_ > cap_) Grow(o.size_);
    if (o.size_) std::memcpy(data_.get(), o.data_.get(), o.size_);
    size_ = o.size_;
  }
  std::unique_ptr<uint8_t[]> data_;
  size_t size_ = 0, cap_ = 0;
};

class FrameBuilder {
 public:
  void Begin(uint64_t tick) {
    tick_ = tick;
    direct_record_ = 0;   // S103 PF2
    epoch_ = g_fb_epoch_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    dlss_ = DlssInputs{};  // S10b
    const bool fast = g_fb_fast.load(std::memory_order_relaxed);  // S102 T4: read every frame, latched for this one
    const bool plant = fast && g_fb_fast_plant.load(std::memory_order_relaxed);  // TEST ONLY
    if (fast) g_fb_fast_frames.fetch_add(1, std::memory_order_relaxed);
    out_cap_h_ = g_out_cap_h.load(std::memory_order_relaxed);        // S102 G2: tools (rplay / fview) set the atomics; the game's live.cpp overrides with SetOutput
    out_bilinear_ = g_out_bilinear.load(std::memory_order_relaxed);
    present_fetch_ = g_present_fetch.load(std::memory_order_relaxed);  // S102 R24-A: tools set the atomic; the game's live.cpp overrides with SetPresentFetch
    colour_cuts_ = colour_cuts::g_colour_cuts.load(std::memory_order_relaxed);  // S102 G10: tools set the atomic; the game's live.cpp overrides with SetColourCuts
    arena_overdraw_ = colour_cuts::g_arena_overdraw.load(std::memory_order_relaxed);  // S102 C4 (same: SetArenaOverdraw)
    half_transparents_ = colour_cuts::g_half_transparents.load(std::memory_order_relaxed);  // S102 C4 (SetHalfTransparents)
    arena_fragments_ = arena_fragments::g_mask.load(std::memory_order_relaxed);  // S102 R18-B3 (SetArenaFragments)
    pack_once_ = g_pack_once.load(std::memory_order_relaxed);     // S102 T5
    group_min_run_ = uint8_t(std::clamp(g_gpu_group_stamps.load(std::memory_order_relaxed), 0, 255));   // S102 R24-F: tools set the atomic; the game's live.cpp overrides with SetGroupStamps
    group_cur_ = kGrpOther;
    draw_groups_.clear();
    if (pack_once_) g_pack_once_frames.fetch_add(1, std::memory_order_relaxed);
    ops_.clear();
    targets_.clear();
    clears_.clear();
    draws_.clear();
    copies_.clear();
    presents_.clear();
    dispatches_.clear();   // S102 L1b
    states_.Clear(fast, plant);
    dynamics_.Clear(fast, plant);
    samplers_.Clear(fast, plant);
    texture_sets_.Clear(fast, plant);
    blend_ranges_.clear();
    if (blend_tags_) blend_occ_.Reset(256);   // S103 P7 tween: the occurrence counts of own blend keys are per frame
    constants_.clear();
    geometry_.clear();
  }

  void SetTargets(const TargetsOp& op) { Push(OpKind::Targets, targets_, op); }
  void Clear(const ClearOp& op) { Push(OpKind::Clear, clears_, op); }
  void Draw(const DrawOp& op) {
    Push(OpKind::Draw, draws_, op);
    if (group_min_run_) draw_groups_.push_back(group_cur_);   // S102 R24-F: the draw's group = the one set last (the binder sets it per source op, and per draw in BindDraw)
  }
  // S103 GSTP: a draw with its OWN group (no sticky state: an unlabelled draw can never inherit the previous draw's group). The scene front-end, crowd, particles, 2D rows, maps and post
  // use this one (lint tools\scene\gstp_lint.py: every .Draw( in src\renderer_scene carries a group); the stream binder keeps the sticky SetGroup + Draw(op).
  void Draw(const DrawOp& op, uint8_t group) {
    Push(OpKind::Draw, draws_, op);
    if (group_min_run_) draw_groups_.push_back(group);
  }
  // S102 R24-F: the group the NEXT Draw is tagged with (group_stamps.h); a no-op cost when the switch is off (one compare in Draw).
  bool GroupsOn() const { return group_min_run_ != 0; }
  void SetGroup(uint8_t group) { group_cur_ = group; }
  void SetGroupStamps(uint32_t min_run) { group_min_run_ = uint8_t(std::min<uint32_t>(min_run, 255)); }
  // S9b fix (Jarrod's phone test of the decoupled 120: blocky, wrongly turned smoke): the index the next Draw gets, and a draw made
  // never-interpolated afterwards (its key ambiguous in the frame).
  uint32_t DrawCount() const { return uint32_t(draws_.size()); }
  void ClearBlendKey(uint32_t draw) {
    if (draw < draws_.size()) draws_[draw].blend_key_low = draws_[draw].blend_key_high = 0;
  }
  void Copy(const CopyOp& op) { Push(OpKind::Copy, copies_, op); }
  void Present(const PresentOp& op) { Push(OpKind::Present, presents_, op); }
  void Dispatch(const DispatchOp& op) { Push(OpKind::Dispatch, dispatches_, op); }   // S102 L1b

  uint32_t State(const DrawState& value) { return states_.Add(value); }
  uint32_t Dynamic(const DynamicState& value) { return dynamics_.Add(value); }
  uint32_t Sampler(const SamplerDesc& value) { return samplers_.Add(value); }
  uint32_t Textures(const TextureSet& value) { return texture_sets_.Add(value); }

  // Returns the byte offset to put in a DrawOp.
  uint32_t PushConstants(const void* data, uint32_t bytes) { return Append(constants_, data, bytes, kConstantAlign); }
  // S102 T5: reserve `bytes` of the constants block, aligned like PushConstants and zeroed, and return a pointer to pack straight into (valid until the
  // next Push / Reserve). The same bytes in the same place PushConstants gives, without the stack block and its copy.
  uint8_t* ReserveConstants(uint32_t bytes, uint32_t& offset) {
    const size_t at = (constants_.size() + kConstantAlign - 1) & ~size_t(kConstantAlign - 1);
    constants_.resize(at + bytes);  // new bytes (the alignment gap too) are value-initialised: zero
    offset = uint32_t(at);
    return constants_.data() + at;
  }
  bool PackOnce() const { return pack_once_; }  // T5: latched in Begin, the same for the whole frame
  uint32_t PushGeometry(const void* data, uint32_t bytes, uint32_t align = 4) {   // S103 PF2: one memcpy, no zero fill of the block
    uint32_t offset = 0;
    uint8_t* at = geometry_.Reserve(bytes, align, offset);
    if (bytes) std::memcpy(at, data, bytes);
    return offset;
  }
  void NoteDirectRecord(uint64_t serial_plus_1) { direct_record_ = serial_plus_1; }   // S103 PF2: a draw of this frame reads a host-visible vertex copy; the Render serial at Record, + 1
  uint32_t GeometryBytes() const { return uint32_t(geometry_.size()); }
  uint32_t ConstantsBytes() const { return uint32_t(constants_.size()); }   // S103 PVR: with GeometryBytes, how much of the frame ring this frame has used so far   // S103 PVR: what this frame's geometry blob holds so far (a producer with a budget asks before it pushes)
  uint64_t Epoch() const { return epoch_; }   // S103 PVR: this Begin's process-unique id (never 0 after a Begin)
  // Returns the index for DrawOp::blend_first.
  uint32_t PushBlendRanges(const BlendRange* ranges, uint32_t count) {
    const uint32_t first = uint32_t(blend_ranges_.size());
    blend_ranges_.insert(blend_ranges_.end(), ranges, ranges + count);
    return first;
  }

  // S103 P7 (the scene host's decoupled 120 without the stream): an OWN draw (the scene front-end, the crowd, the particles) is tagged so rdr::TickBlender finds it again in the previous tick.
  // The stream path's keys come from the binder (BlendRanges); the host has no binder, so the drawer that makes the draw gives it a base key (what it is: object id, asset, mesh, shader pair) and the
  // ranges of its vertex constants the shader build marked interpolable (fx::ConstantRange::blend: transforms, camera, bones). The Nth draw of one base key in a frame gets occurrence N (the binder's rule).
  // Off unless the owner switches it on (SetBlendTags): the stream path never does, so nothing here costs a frame there.
  void SetBlendTags(bool on) { blend_tags_ = on; }
  bool BlendTagsOn() const { return blend_tags_; }
  void TagBlend(DrawOp& op, const BlendRange* ranges, uint32_t count, uint64_t base_key) {
    if (!blend_tags_ || !count) return;
    const uint32_t occurrence = blend_occ_.Bump(base_key);
    uint64_t key = (base_key ^ occurrence) * 0x100000001b3ull;
    key |= kOwnBlendKeyBit;   // (never 0, never a binder key)
    op.blend_first = PushBlendRanges(ranges, count);
    op.blend_count = uint16_t(count);
    op.blend_key_low = uint32_t(key);
    op.blend_key_high = uint32_t(key >> 32);
  }

  // Settings parity S10b: this frame's DLSS inputs (binder.h, at the present).
  void SetDlss(const DlssInputs& dlss) { dlss_ = dlss; }
  // S102 G2: the output levers for this frame (live.cpp, read from the S0-4 table at record time).
  void SetOutput(uint32_t cap_h, bool bilinear) { out_cap_h_ = cap_h, out_bilinear_ = bilinear; }
  void SetPresentFetch(bool on) { present_fetch_ = on; }  // S102 R24-A
  void SetColourCuts(uint32_t cuts) { colour_cuts_ = cuts; }  // S102 G10
  void SetArenaOverdraw(bool on) { arena_overdraw_ = on; }  // S102 C4
  bool ArenaOverdraw() const { return arena_overdraw_; }  // S102 C4: the binder proves texture alpha only when on
  void SetHalfTransparents(bool on) { half_transparents_ = on; }  // S102 C4
  void SetArenaFragments(uint32_t mask) { arena_fragments_ = mask; }  // S102 R18-B3
  uint32_t ArenaFragments() const { return arena_fragments_; }  // S102 R18-B3: the binder proves exact alpha only when a bit is on
  uint32_t ColourCuts() const { return colour_cuts_; }  // S102 G10 (review F2): the binder proves alpha only when the cut is on

  Frame Finish() const {
    Frame frame;
    frame.tick = tick_;
    frame.direct_record = direct_record_;   // S103 PF2
    frame.ops = MakeSpan(ops_);
    frame.targets = MakeSpan(targets_);
    frame.clears = MakeSpan(clears_);
    frame.draws = MakeSpan(draws_);
    frame.copies = MakeSpan(copies_);
    frame.presents = MakeSpan(presents_);
    frame.dispatches = MakeSpan(dispatches_);   // S102 L1b
    frame.states = MakeSpan(states_.items);
    frame.dynamics = MakeSpan(dynamics_.items);
    frame.samplers = MakeSpan(samplers_.items);
    frame.texture_sets = MakeSpan(texture_sets_.items);
    frame.blend_ranges = MakeSpan(blend_ranges_);
    frame.constants = MakeSpan(constants_);
    frame.geometry = Span<uint8_t>{geometry_.data(), uint32_t(geometry_.size())};
    frame.dlss = dlss_;  // S10b
    frame.out_cap_h = out_cap_h_, frame.out_bilinear = out_bilinear_;  // S102 G2
    frame.present_fetch = present_fetch_;  // S102 R24-A
    frame.colour_cuts = colour_cuts_;  // S102 G10
    frame.arena_overdraw = arena_overdraw_, frame.half_transparents = half_transparents_;  // S102 C4
    frame.arena_fragments = arena_fragments_;  // S102 R18-B3
    frame.group_min_run = group_min_run_;      // S102 R24-F: only a frame whose draws all carry a group (an equal count) is cut into segments
    if (group_min_run_ && draw_groups_.size() == draws_.size()) frame.draw_groups = MakeSpan(draw_groups_);
    return frame;
  }

 private:
  DlssInputs dlss_;  // S10b
  uint32_t out_cap_h_ = 0;      // S102 G2
  uint32_t colour_cuts_ = 0;    // S102 G10
  bool arena_overdraw_ = false, half_transparents_ = false;  // S102 C4
  uint32_t arena_fragments_ = 0;  // S102 R18-B3
  bool out_bilinear_ = false;
  bool present_fetch_ = false;  // S102 R24-A
  uint8_t group_min_run_ = 0, group_cur_ = kGrpOther;   // S102 R24-F
  std::vector<uint8_t> draw_groups_;
  // S102 T4: two ways to de-duplicate, chosen per frame in Begin (g_fb_fast). Ids are insertion order and equality is an exact memcmp in both, so
  // the output is the same either way; only the cost differs. Legacy: byte-wise FNV into a node-based multimap freed every frame. Fast: a
  // word-wise hash into an open-addressing table whose storage stays across frames (Clear bumps a generation).
  template <class T>
  struct Dedup {
    std::vector<T> items;
    std::unordered_multimap<uint64_t, uint32_t> index;  // legacy
    struct Slot {
      uint64_t key;
      uint32_t id;
      uint32_t gen;
    };
    std::vector<Slot> slots;     // fast: power of two, a slot is live when gen == gen_
    std::vector<uint64_t> keys;  // fast: keys[id], for a rehash
    uint32_t gen_ = 1;
    bool fast_ = false;
    bool plant_ = false;  // TEST ONLY (g_fb_fast_plant)
    void Clear(bool fast, bool plant) {
      fast_ = fast;
      plant_ = plant;
      items.clear();
      keys.clear();
      if (fast) {
        if (!index.empty()) std::unordered_multimap<uint64_t, uint32_t>().swap(index);
        if (++gen_ == 0) {  // wrapped: really clear
          for (Slot& s : slots) s.gen = 0;
          gen_ = 1;
        }
      } else {
        index.clear();
      }
    }
    uint32_t Add(const T& value) { return fast_ ? AddFast(value) : AddLegacy(value); }
    uint32_t AddFast(const T& value) {
      if (slots.empty() || (items.size() + 1) * 2 > slots.size()) Grow();
      const uint64_t key = plant_ ? HashWords(&value, sizeof(T) / 2) : HashWords(&value, sizeof(T));  // plant: ignores the whole second half (the last words, and with them real differences)
      const size_t mask = slots.size() - 1;
      for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
        Slot& s = slots[i];
        if (s.gen != gen_) {
          const uint32_t id = uint32_t(items.size());
          items.push_back(value);
          keys.push_back(key);
          s = Slot{key, id, gen_};
          return id;
        }
#ifdef RDR_FB_PLANT_NOCMP  // TEST ONLY: the key alone decides equality (with RDR_FB_PLANT_LASTWORD it merges states that differ in the last word)
        if (s.key == key) return s.id;
#else
        if (s.key == key && (plant_ || std::memcmp(&items[s.id], &value, sizeof(T)) == 0)) return s.id;  // plant: the key alone decides
#endif
      }
    }
    void Grow() {
      const size_t cap = slots.empty() ? 256 : slots.size() * 2;
      std::vector<Slot> next(cap, Slot{0, 0, 0});
      const size_t mask = cap - 1;
      for (uint32_t id = 0; id < keys.size(); ++id) {
        size_t i = size_t(keys[id]) & mask;
        while (next[i].gen == gen_) i = (i + 1) & mask;
        next[i] = Slot{keys[id], id, gen_};
      }
      slots.swap(next);
    }
    uint32_t AddLegacy(const T& value) {
      const uint64_t key = Hash(&value, sizeof(T));
      auto range = index.equal_range(key);
      for (auto it = range.first; it != range.second; ++it) {
        if (std::memcmp(&items[it->second], &value, sizeof(T)) == 0) return it->second;
      }
      const uint32_t id = uint32_t(items.size());
      items.push_back(value);
      index.emplace(key, id);
      return id;
    }
  };

  // S102 T4: a hash over 8-byte words, two independent lanes and a final avalanche; every byte is read (a short tail is zero-padded into a word).
  // The offline test (tools/fb/fb_test.cpp) measures its distribution against the byte-wise one. Defined here, declared private beside Hash.
  static uint64_t HashWords(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    constexpr uint64_t K1 = 0x9E3779B97F4A7C15ull, K2 = 0xC2B2AE3D27D4EB4Full;
#if defined(RDR_FB_PLANT_LASTWORD) || defined(RDR_FB_PLANT_NOCMP)
#ifndef RDR_FB_UNIT_TEST_BUILD
#error RDR_FB_PLANT_* macros belong to tools/fb/fb_test.cpp only
#endif
#endif
#ifdef RDR_FB_PLANT_LASTWORD  // TEST ONLY (tools/fb/fb_test.cpp): the planted twin, a hash that ignores the last word
    if (bytes >= 8) bytes -= 8;
#endif
    uint64_t a = 0x243F6A8885A308D3ull ^ bytes, b = 0x13198A2E03707344ull;
    size_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
      uint64_t w0, w1;
      std::memcpy(&w0, p + i, 8);
      std::memcpy(&w1, p + i + 8, 8);
      a = (a ^ w0) * K1;
      a ^= a >> 32;
      b = (b ^ w1) * K2;
      b ^= b >> 32;
    }
    for (; i < bytes; i += 8) {
      uint64_t w = 0;
      std::memcpy(&w, p + i, bytes - i < 8 ? bytes - i : 8);
      a = (a ^ w) * K1;
      a ^= a >> 32;
    }
    uint64_t h = a ^ (b * K2);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
  }

  static uint64_t Hash(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < bytes; ++i) {
      h ^= p[i];
      h *= 0x100000001b3ull;
    }
    return h;
  }

  template <class T>
  static Span<T> MakeSpan(const std::vector<T>& items) {
    return Span<T>{items.data(), uint32_t(items.size())};
  }

  template <class T>
  void Push(OpKind kind, std::vector<T>& list, const T& op) {
    ops_.push_back(Op{kind, uint32_t(list.size())});
    list.push_back(op);
  }

  static uint32_t Append(std::vector<uint8_t>& blob, const void* data, uint32_t bytes, uint32_t align) {
    const size_t offset = (blob.size() + align - 1) & ~size_t(align - 1);
    blob.resize(offset + bytes);
    std::memcpy(blob.data() + offset, data, bytes);
    return uint32_t(offset);
  }

  uint64_t epoch_ = 0;   // S103 PVR
  uint64_t direct_record_ = 0;   // S103 PF2
  bool pack_once_ = false;  // S102 T5: this frame's mode (Begin)
  uint64_t tick_ = 0;
  std::vector<Op> ops_;
  std::vector<TargetsOp> targets_;
  std::vector<ClearOp> clears_;
  std::vector<DrawOp> draws_;
  std::vector<CopyOp> copies_;
  std::vector<PresentOp> presents_;
  std::vector<DispatchOp> dispatches_;   // S102 L1b
  Dedup<DrawState> states_;
  Dedup<DynamicState> dynamics_;
  Dedup<SamplerDesc> samplers_;
  Dedup<TextureSet> texture_sets_;
  std::vector<BlendRange> blend_ranges_;
  rdr_scene::fast::FlatU64Map blend_occ_;   // S103 P7: TagBlend's occurrence per base key (this frame)
  bool blend_tags_ = false;
  std::vector<uint8_t> constants_;
  GeometryBlob geometry_;   // S103 PF2: no zero fill on growth
};

}  // namespace rdr
