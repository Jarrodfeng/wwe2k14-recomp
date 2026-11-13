// Builds a Frame from calls. State tables are de-duplicated as they are filled. All storage is reused from frame to frame.
#pragma once
#include <cstring>
#include <unordered_map>
#include <vector>

#include "api.h"

namespace rdr {

class FrameBuilder {
 public:
  void Begin(uint64_t tick) {
    tick_ = tick;
    dlss_ = DlssInputs{};  // S10b
    ops_.clear();
    targets_.clear();
    clears_.clear();
    draws_.clear();
    copies_.clear();
    presents_.clear();
    states_.Clear();
    dynamics_.Clear();
    samplers_.Clear();
    texture_sets_.Clear();
    blend_ranges_.clear();
    constants_.clear();
    geometry_.clear();
  }

  void SetTargets(const TargetsOp& op) { Push(OpKind::Targets, targets_, op); }
  void Clear(const ClearOp& op) { Push(OpKind::Clear, clears_, op); }
  void Draw(const DrawOp& op) { Push(OpKind::Draw, draws_, op); }
  // S9b fix (Jarrod's phone test of the decoupled 120: blocky, wrongly turned smoke): the index the next Draw gets, and a draw made
  // never-interpolated afterwards (its key ambiguous in the frame).
  uint32_t DrawCount() const { return uint32_t(draws_.size()); }
  void ClearBlendKey(uint32_t draw) {
    if (draw < draws_.size()) draws_[draw].blend_key_low = draws_[draw].blend_key_high = 0;
  }
  void Copy(const CopyOp& op) { Push(OpKind::Copy, copies_, op); }
  void Present(const PresentOp& op) { Push(OpKind::Present, presents_, op); }

  uint32_t State(const DrawState& value) { return states_.Add(value); }
  uint32_t Dynamic(const DynamicState& value) { return dynamics_.Add(value); }
  uint32_t Sampler(const SamplerDesc& value) { return samplers_.Add(value); }
  uint32_t Textures(const TextureSet& value) { return texture_sets_.Add(value); }

  // Returns the byte offset to put in a DrawOp.
  uint32_t PushConstants(const void* data, uint32_t bytes) { return Append(constants_, data, bytes, kConstantAlign); }
  uint32_t PushGeometry(const void* data, uint32_t bytes, uint32_t align = 4) { return Append(geometry_, data, bytes, align); }
  // Returns the index for DrawOp::blend_first.
  uint32_t PushBlendRanges(const BlendRange* ranges, uint32_t count) {
    const uint32_t first = uint32_t(blend_ranges_.size());
    blend_ranges_.insert(blend_ranges_.end(), ranges, ranges + count);
    return first;
  }

  // Settings parity S10b: this frame's DLSS inputs (binder.h, at the present).
  void SetDlss(const DlssInputs& dlss) { dlss_ = dlss; }

  Frame Finish() const {
    Frame frame;
    frame.tick = tick_;
    frame.ops = MakeSpan(ops_);
    frame.targets = MakeSpan(targets_);
    frame.clears = MakeSpan(clears_);
    frame.draws = MakeSpan(draws_);
    frame.copies = MakeSpan(copies_);
    frame.presents = MakeSpan(presents_);
    frame.states = MakeSpan(states_.items);
    frame.dynamics = MakeSpan(dynamics_.items);
    frame.samplers = MakeSpan(samplers_.items);
    frame.texture_sets = MakeSpan(texture_sets_.items);
    frame.blend_ranges = MakeSpan(blend_ranges_);
    frame.constants = MakeSpan(constants_);
    frame.geometry = MakeSpan(geometry_);
    frame.dlss = dlss_;  // S10b
    return frame;
  }

 private:
  DlssInputs dlss_;  // S10b
  template <class T>
  struct Dedup {
    std::vector<T> items;
    std::unordered_multimap<uint64_t, uint32_t> index;
    void Clear() {
      items.clear();
      index.clear();
    }
    uint32_t Add(const T& value) {
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

  uint64_t tick_ = 0;
  std::vector<Op> ops_;
  std::vector<TargetsOp> targets_;
  std::vector<ClearOp> clears_;
  std::vector<DrawOp> draws_;
  std::vector<CopyOp> copies_;
  std::vector<PresentOp> presents_;
  Dedup<DrawState> states_;
  Dedup<DynamicState> dynamics_;
  Dedup<SamplerDesc> samplers_;
  Dedup<TextureSet> texture_sets_;
  std::vector<BlendRange> blend_ranges_;
  std::vector<uint8_t> constants_;
  std::vector<uint8_t> geometry_;
};

}  // namespace rdr
